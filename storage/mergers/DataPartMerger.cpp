#include "DataPartMerger.h"

#include <unordered_set>
#include <vector>

#include <range/v3/view/enumerate.hpp>

#include "datapart/DataPart.h"
#include "writers/DataPartBuilder.h"
#include "versioning/CommitBuilder.h"
#include "datapart/EdgeContainer.h"
#include "datapart/NodeContainer.h"
#include "versioning/TombstoneRanges.h"
#include "metadata/PropertyTypeDispatcher.h"
#include "properties/PropertyManager.h"
#include "reader/GraphReader.h"

#include "ID.h"

using namespace db;
namespace rg = ranges;
namespace rv = rg::views;

DataPartMerger::DataPartMerger(const CommitData* commitData,
                               MetadataBuilder& metadataBuilder)
    : _graphView(commitData),
    _metadataBuilder(metadataBuilder)
{
}

std::unique_ptr<DataPartBuilder> DataPartMerger::merge(DataPartSpan dataParts) const {
    const auto& graphReader = _graphView.read();
    const auto& tombstones = _graphView.tombstones();

    const size_t nodeCount = graphReader.getNodeCount();
    const size_t edgeCount = graphReader.getEdgeCount();

    std::unique_ptr<DataPartBuilder> datapartBuilder = DataPartBuilder::prepareMerge(_metadataBuilder, _graphView);

    std::vector<LabelSetHandle>& labelSets = datapartBuilder->coreNodeLabelSets();
    std::vector<EdgeRecord>& edges = datapartBuilder->edges();
    PropertyManager& nodePropertyManager = *datapartBuilder->nodeProperties();
    PropertyManager& edgePropertyManager = *datapartBuilder->edgeProperties();

    // nodeIDVal will let us keep track of the current NodeID we are on
    // we can use this to check the tombstones as well as build the custom
    // tmpNodeIDs vector we will pass to datapart::load()
    std::vector<NodeID>& nodeVector = datapartBuilder->getTmpNodeIDs();
    NodeID nodeIDVal(0);

    nodeVector.reserve(nodeCount);
    labelSets.reserve(nodeCount);
    edges.resize(edgeCount);

    // Map to store the number of entries for each property
    std::unordered_map<PropertyTypeID, size_t> _nodePropertiesCount;
    std::unordered_map<PropertyTypeID, size_t> _edgePropertiesCount;

    // Maps to store the ranges of the property container to keep in every datapart
    std::vector<std::unordered_map<PropertyTypeID, TombstoneRanges>> nodeRangesPerPart(dataParts.size());
    std::vector<std::unordered_map<PropertyTypeID, TombstoneRanges>> edgeRangesPerPart(dataParts.size());

    // We keep a set emptyProperties to skip if they do not have values left
    // after deletion
    std::unordered_set<PropertyTypeID> emptyNodeProperties;
    std::unordered_set<PropertyTypeID> emptyEdgeProperties;
    // Count the values of each property to keep, and store the ranges of each container to keep.
    // A value is kept if a read returns it: its entity is not deleted, no newer datapart holds a
    // value or an explicit NULL for it, and it is the entry its own datapart reads. A SET adds
    // its value to a newer datapart, so the older ones of that entity are dropped here.
    // The dataparts are visited newest first, the order in which reads look them up.
    std::unordered_map<PropertyTypeID, std::unordered_set<EntityID>> nodeIDsSeen;
    std::unordered_map<PropertyTypeID, std::unordered_set<EntityID>> edgeIDsSeen;

    const auto keepReadValues = [&]<TypedInternalID IDT>(const PropertyManager& properties,
                                                        std::unordered_map<PropertyTypeID, TombstoneRanges>& rangesOfPart,
                                                        std::unordered_map<PropertyTypeID, std::unordered_set<EntityID>>& idsSeen,
                                                        std::unordered_map<PropertyTypeID, size_t>& counts) {
        for (const auto& [propertyID, propertyContainer] : properties) {
            const auto& ids = propertyContainer->ids();
            auto& seen = idsSeen[propertyID];

            std::vector<bool> superseded(ids.size());
            for (size_t i = 0; i < ids.size(); i++) {
                superseded[i] = seen.contains(ids[i]) || !propertyContainer->isReadEntry(i);
            }

            const auto [it, inserted] = rangesOfPart.emplace(propertyID, tombstones);
            auto& keptRanges = it->second;

            keptRanges.template populateRanges<IDT>(ids, superseded);
            counts[propertyID] += keptRanges.getNumRemainingEntities();

            seen.insert(ids.begin(), ids.end());
            seen.insert(propertyContainer->nullIds().begin(), propertyContainer->nullIds().end());
        }
    };

    for (size_t idx = dataParts.size(); idx-- > 0;) {
        const auto& dataPart = dataParts[idx];

        keepReadValues.template operator()<NodeID>(dataPart->nodeProperties(), nodeRangesPerPart[idx], nodeIDsSeen, _nodePropertiesCount);
        keepReadValues.template operator()<EdgeID>(dataPart->edgeProperties(), edgeRangesPerPart[idx], edgeIDsSeen, _edgePropertiesCount);
    }

    const auto& prevMetadata = _graphView.metadata();
    size_t edgeContainerOffset = 0;

    // Loop Over The Dataparts and Start Merging Containers
    for (const auto& [idx, dataPart] : dataParts | rv::enumerate) {
        // Create labelSets pointing to the current commits metatdata.
        // As the nodeIDs of the original dataPart span are sorted in order, when we insert the
        // labelSets into the new dataPart they follow an ascending insertion nodeID order -
        // this follows the pattern expected by dataPart::load() and will allow us to directly copy
        // other datapart containers without needing to map the nodeIDs.
        for (const auto& nodeRecord : dataPart->nodes().records()) {
            if (!tombstones.containsNode(nodeIDVal)) {
                labelSets.emplace_back(*prevMetadata.labelsets().getValue(nodeRecord._labelset.getID()));
                nodeVector.push_back(nodeIDVal);
            }
            ++nodeIDVal;
        }

        // Copy Edges as-is (using the tombstone ranges), (as mentioned above)the nodeIDs referred to by the edges
        // follow the insertion order of the labelSets above - so these can be sorted by dataPart::load().
        TombstoneRanges deletedEdges(tombstones);
        deletedEdges.populateRanges(dataPart->edges()._outEdges);
        for (const auto& [start, size] : deletedEdges) {
            std::memcpy(edges.data() + edgeContainerOffset, dataPart->edges()._outEdges.data() + start, size * sizeof(EdgeRecord));
            edgeContainerOffset += size;
        }

        // Go over all the existing node properties and copy them into the single datapart's node container. Similar to the edges the nodeIDs here
        // follow the order of insertion of the labelsets so they will be sorted appropriately in the load() function.
        for (const auto& [propertyID, propertyContainer] : dataPart->nodeProperties()) {
            if (emptyNodeProperties.contains(propertyID)) {
                continue;
            }
            const auto process = [&]<SupportedType Type> {
                if constexpr (std::is_same_v<Type, types::String>) {
                    if (!nodePropertyManager.hasPropertyType(propertyID)) {
                        if (_nodePropertiesCount[propertyID] == 0) {
                            emptyNodeProperties.insert(propertyID);
                            return;
                        }
                        nodePropertyManager.registerPropertyType<types::String>(propertyID);

                        auto& propertyContainer = nodePropertyManager.getMutableContainer<types::String>(propertyID);
                        propertyContainer._values._views.resize(_nodePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_nodePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _nodePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::String>();
                    auto& newContainer = nodePropertyManager.getMutableContainer<types::String>(propertyID);

                    // since the string_view values are just copied into another string_view on the sort
                    // we can just copy the id's and the corresponding string_view that still point to
                    // the string buckets in the unmerged datapart commit. This data is guaranteed to
                    // exist until our merged datapart is fully created. This allows us to avoid unecessary
                    // copies of all the strings, and avoid having to do some not-so-nice pointer transalations

                    const TombstoneRanges& deletedNodeIDs = nodeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : deletedNodeIDs) {
                        std::memcpy(newContainer._values._views.data() + _nodePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(std::string_view)));
                        std::memcpy(newContainer.getMutableIDs().data() + _nodePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _nodePropertiesCount[propertyID] += size;
                    }

                } else if constexpr (std::is_same_v<Type, types::Embedding>) {
                    if (!nodePropertyManager.hasPropertyType(propertyID)) {
                        if (_nodePropertiesCount[propertyID] == 0) {
                            emptyNodeProperties.insert(propertyID);
                            return;
                        }
                        const auto& oldEmbContainer = propertyContainer->cast<types::Embedding>();
                        const size_t dimension = oldEmbContainer.getRawContainer().getDimension();
                        nodePropertyManager.registerEmbeddingPropertyType(propertyID, dimension);

                        auto& newEmbContainer = nodePropertyManager.getMutableContainer<types::Embedding>(propertyID);
                        newEmbContainer._values._views.resize(_nodePropertiesCount[propertyID]);
                        newEmbContainer.getMutableIDs().resize(_nodePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _nodePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::Embedding>();
                    auto& newContainer = nodePropertyManager.getMutableContainer<types::Embedding>(propertyID);

                    const TombstoneRanges& keptRanges = nodeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : keptRanges) {
                        std::memcpy(newContainer._values._views.data() + _nodePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(std::span<const float>)));
                        std::memcpy(newContainer.getMutableIDs().data() + _nodePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _nodePropertiesCount[propertyID] += size;
                    }

                } else if constexpr (std::is_same_v<Type, types::List>) {
                    if (!nodePropertyManager.hasPropertyType(propertyID)) {
                        if (_nodePropertiesCount[propertyID] == 0) {
                            emptyNodeProperties.insert(propertyID);
                            return;
                        }
                        nodePropertyManager.registerPropertyType<types::List>(propertyID);

                        auto& propertyContainer = nodePropertyManager.getMutableContainer<types::List>(propertyID);
                        propertyContainer._values._views.resize(_nodePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_nodePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _nodePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::List>();
                    auto& newContainer = nodePropertyManager.getMutableContainer<types::List>(propertyID);

                    const TombstoneRanges& keptRanges = nodeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : keptRanges) {
                        std::memcpy(newContainer._values._views.data() + _nodePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(ListView)));
                        std::memcpy(newContainer.getMutableIDs().data() + _nodePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _nodePropertiesCount[propertyID] += size;
                    }

                } else if constexpr (std::is_same_v<Type, types::Map>) {
                    if (!nodePropertyManager.hasPropertyType(propertyID)) {
                        if (_nodePropertiesCount[propertyID] == 0) {
                            emptyNodeProperties.insert(propertyID);
                            return;
                        }
                        nodePropertyManager.registerPropertyType<types::Map>(propertyID);

                        auto& propertyContainer = nodePropertyManager.getMutableContainer<types::Map>(propertyID);
                        propertyContainer._values._views.resize(_nodePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_nodePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _nodePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::Map>();
                    auto& newContainer = nodePropertyManager.getMutableContainer<types::Map>(propertyID);

                    const TombstoneRanges& keptRanges = nodeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : keptRanges) {
                        std::memcpy(newContainer._values._views.data() + _nodePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(MapView)));
                        std::memcpy(newContainer.getMutableIDs().data() + _nodePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _nodePropertiesCount[propertyID] += size;
                    }

                } else {
                    if (!nodePropertyManager.hasPropertyType(propertyID)) {
                        if (_nodePropertiesCount[propertyID] == 0) {
                            emptyNodeProperties.insert(propertyID);
                            return;
                        }
                        nodePropertyManager.registerPropertyType<Type>(propertyID);

                        auto& propertyContainer = nodePropertyManager.getMutableContainer<Type>(propertyID);
                        propertyContainer.values().resize(_nodePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_nodePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _nodePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<Type>();
                    auto& newContainer = nodePropertyManager.getMutableContainer<Type>(propertyID);

                    const TombstoneRanges& deletedNodeIDs = nodeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : deletedNodeIDs) {
                        std::memcpy(newContainer.values().data() + _nodePropertiesCount[propertyID],
                                    oldContainer.values().data() + start,
                                    (size * sizeof(typename Type::Primitive)));
                        std::memcpy(newContainer.getMutableIDs().data() + _nodePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _nodePropertiesCount[propertyID] += size;
                    }
                }
            };
            PropertyTypeDispatcher {propertyContainer->getValueType()}.execute(process);
        }

        // copy over the edge properties
        for (const auto& [propertyID, propertyContainer] : dataPart->edgeProperties()) {
            if (emptyEdgeProperties.contains(propertyID)) {
                continue;
            }

            const auto process = [&]<SupportedType Type> {
                if constexpr (std::is_same_v<Type, types::String>) {
                    if (!edgePropertyManager.hasPropertyType(propertyID)) {
                        if (_edgePropertiesCount[propertyID] == 0) {
                            emptyEdgeProperties.insert(propertyID);
                            return;
                        }

                        edgePropertyManager.registerPropertyType<types::String>(propertyID);

                        auto& propertyContainer = edgePropertyManager.getMutableContainer<types::String>(propertyID);
                        propertyContainer._values._views.resize(_edgePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_edgePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _edgePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::String>();
                    auto& newContainer = edgePropertyManager.getMutableContainer<types::String>(propertyID);

                    const TombstoneRanges& deletedEdgeIDs = edgeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : deletedEdgeIDs) {
                        std::memcpy(newContainer._values._views.data() + _edgePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(std::string_view)));
                        std::memcpy(newContainer.getMutableIDs().data() + _edgePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _edgePropertiesCount[propertyID] += size;
                    }

                } else if constexpr (std::is_same_v<Type, types::Embedding>) {
                    if (!edgePropertyManager.hasPropertyType(propertyID)) {
                        if (_edgePropertiesCount[propertyID] == 0) {
                            emptyEdgeProperties.insert(propertyID);
                            return;
                        }
                        const auto& oldEmbContainer = propertyContainer->cast<types::Embedding>();
                        const size_t dimension = oldEmbContainer.getRawContainer().getDimension();
                        edgePropertyManager.registerEmbeddingPropertyType(propertyID, dimension);

                        auto& newEmbContainer = edgePropertyManager.getMutableContainer<types::Embedding>(propertyID);
                        newEmbContainer._values._views.resize(_edgePropertiesCount[propertyID]);
                        newEmbContainer.getMutableIDs().resize(_edgePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _edgePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::Embedding>();
                    auto& newContainer = edgePropertyManager.getMutableContainer<types::Embedding>(propertyID);

                    const TombstoneRanges& keptRanges = edgeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : keptRanges) {
                        std::memcpy(newContainer._values._views.data() + _edgePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(std::span<const float>)));
                        std::memcpy(newContainer.getMutableIDs().data() + _edgePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _edgePropertiesCount[propertyID] += size;
                    }

                } else if constexpr (std::is_same_v<Type, types::List>) {
                    if (!edgePropertyManager.hasPropertyType(propertyID)) {
                        if (_edgePropertiesCount[propertyID] == 0) {
                            emptyEdgeProperties.insert(propertyID);
                            return;
                        }
                        edgePropertyManager.registerPropertyType<types::List>(propertyID);

                        auto& propertyContainer = edgePropertyManager.getMutableContainer<types::List>(propertyID);
                        propertyContainer._values._views.resize(_edgePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_edgePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _edgePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::List>();
                    auto& newContainer = edgePropertyManager.getMutableContainer<types::List>(propertyID);

                    const TombstoneRanges& keptRanges = edgeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : keptRanges) {
                        std::memcpy(newContainer._values._views.data() + _edgePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(ListView)));
                        std::memcpy(newContainer.getMutableIDs().data() + _edgePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _edgePropertiesCount[propertyID] += size;
                    }

                } else if constexpr (std::is_same_v<Type, types::Map>) {
                    if (!edgePropertyManager.hasPropertyType(propertyID)) {
                        if (_edgePropertiesCount[propertyID] == 0) {
                            emptyEdgeProperties.insert(propertyID);
                            return;
                        }
                        edgePropertyManager.registerPropertyType<types::Map>(propertyID);

                        auto& propertyContainer = edgePropertyManager.getMutableContainer<types::Map>(propertyID);
                        propertyContainer._values._views.resize(_edgePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_edgePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _edgePropertiesCount[propertyID] = 0;
                    }

                    auto& oldContainer = propertyContainer->cast<types::Map>();
                    auto& newContainer = edgePropertyManager.getMutableContainer<types::Map>(propertyID);

                    const TombstoneRanges& keptRanges = edgeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : keptRanges) {
                        std::memcpy(newContainer._values._views.data() + _edgePropertiesCount[propertyID],
                                    oldContainer._values._views.data() + start,
                                    (size * sizeof(MapView)));
                        std::memcpy(newContainer.getMutableIDs().data() + _edgePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _edgePropertiesCount[propertyID] += size;
                    }

                } else {
                    if (!edgePropertyManager.hasPropertyType(propertyID)) {
                        if (_edgePropertiesCount[propertyID] == 0) {
                            emptyEdgeProperties.insert(propertyID);
                            return;
                        }
                        edgePropertyManager.registerPropertyType<Type>(propertyID);

                        auto& propertyContainer = edgePropertyManager.getMutableContainer<Type>(propertyID);
                        propertyContainer.values().resize(_edgePropertiesCount[propertyID]);
                        propertyContainer.getMutableIDs().resize(_edgePropertiesCount[propertyID]);

                        // Re-use for offsets;
                        _edgePropertiesCount[propertyID] = 0;
                    }
                    auto& oldContainer = propertyContainer->cast<Type>();
                    auto& newContainer = edgePropertyManager.getMutableContainer<Type>(propertyID);

                    const TombstoneRanges& deletedEdgeIDs = edgeRangesPerPart.at(idx).at(propertyID);
                    for (const auto& [start, size] : deletedEdgeIDs) {
                        std::memcpy(newContainer.values().data() + _edgePropertiesCount[propertyID],
                                    oldContainer.values().data() + start,
                                    (size * sizeof(typename Type::Primitive)));
                        std::memcpy(newContainer.getMutableIDs().data() + _edgePropertiesCount[propertyID],
                                    oldContainer.ids().data() + start,
                                    (size * sizeof(EntityID)));
                        _edgePropertiesCount[propertyID] += size;
                    }
                }
            };
            PropertyTypeDispatcher {propertyContainer->getValueType()}.execute(process);
        }
    }

    return datapartBuilder;
}
