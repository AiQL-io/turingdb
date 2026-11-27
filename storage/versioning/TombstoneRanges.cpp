#include "TombstoneRanges.h"

#include "BioAssert.h"
#include "datapart/EdgeRecord.h"
#include "versioning/Tombstones.h"

using namespace db;

TombstoneRanges::TombstoneRanges(const Tombstones& tombstones)
    :_tombstones(tombstones)
{
}

void TombstoneRanges::populateRanges(const std::vector<EdgeRecord>& unfilteredContainer) {
    size_t i = 0;
    while (i < unfilteredContainer.size()) { // Scan each element in the base column
        // Skip until we find a non-deleted entry
        const bool deleted = _tombstones.contains(unfilteredContainer[i]._edgeID);
        if (deleted) {
            i++;
            continue;
        }

        // We have found a non-deleted entry, scan forward to see how many non-deleted
        // entries we have in a row, and form a range from it
        size_t start = i;
        size_t size = 1;
        i++; // Pre-increment i: we scan the next entry after the non-deleted just found
        while (i < unfilteredContainer.size() && !_tombstones.contains(unfilteredContainer[i]._edgeID)) {
            i++;
            size++;
        }

        _nonDeletedRanges.emplace_back(start, size);
        _numRemainingEntities += size;
    }
}

template <TypedInternalID IDT>
void TombstoneRanges::populateRanges(const std::vector<EntityID>& unfilteredContainer) {
    populateRanges<IDT>(unfilteredContainer, {});
}

template <TypedInternalID IDT>
void TombstoneRanges::populateRanges(const std::vector<EntityID>& unfilteredContainer,
                                     const std::vector<bool>& superseded) {
    const auto kept = [&](size_t i) {
        const bool replaced = i < superseded.size() && superseded[i];
        return !replaced && !_tombstones.contains(static_cast<IDT>(unfilteredContainer[i].getValue()));
    };

    size_t i = 0;
    while (i < unfilteredContainer.size()) { // Scan each element in the base column
        // Skip until we find a kept entry
        if (!kept(i)) {
            i++;
            continue;
        }

        // We have found a kept entry, scan forward to see how many kept
        // entries we have in a row, and form a range from it
        size_t start = i;
        size_t size = 1;
        i++; // Pre-increment i: we scan the next entry after the kept one just found
        while (i < unfilteredContainer.size() && kept(i)) {
            i++;
            size++;
        }

        _nonDeletedRanges.emplace_back(start, size);
        _numRemainingEntities += size;
    }
}

namespace db {
template void TombstoneRanges::populateRanges<NodeID>(const std::vector<EntityID>&);
template void TombstoneRanges::populateRanges<EdgeID>(const std::vector<EntityID>&);
template void TombstoneRanges::populateRanges<NodeID>(const std::vector<EntityID>&, const std::vector<bool>&);
template void TombstoneRanges::populateRanges<EdgeID>(const std::vector<EntityID>&, const std::vector<bool>&);
}
