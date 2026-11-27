import turingdb

t = turingdb.TuringDB()
t.try_reach()

print("Running Wine Ontology Test")

# The Wine Ontology has several hundred labels: more than the 256 a graph once held, fewer than its capacity
res = t.query("CREATE GRAPH mygraph")

t.set_graph("mygraph")
change: int = t.new_change()

t.checkout(change=change)

with open("wine_ontology.cypher") as f:
    wine_create_query = f.read()

t.query(wine_create_query)
t.query("CHANGE SUBMIT")
t.checkout()

labels = t.query("CALL db.labels()")
assert labels.shape[0] > 256, labels.shape

# Past its capacity a graph still refuses the next label, with the same error
res = t.query("CREATE GRAPH fullgraph")

t.set_graph("fullgraph")
change = t.new_change()

t.checkout(change=change)

too_many = "CREATE " + ", ".join(f"(:L{i})" for i in range(4097))

try:
    res = t.query(too_many)
except Exception as e:
    assert (
        str(e)
        == "EXEC_ERROR: Attempted to create LabelID 4096, which exceeds graph label capacity."
    ), str(e)
else:
    assert False  # If an exception wasn't raised: bug


print("Passed Wine Ontology Test")
