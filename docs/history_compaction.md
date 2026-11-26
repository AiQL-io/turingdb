# Compacting a graph's history

A graph directory holds one `commits/<hash>` directory per commit, listed oldest first in
`commitlog`, and one `dataparts/<id>` directory per data part. Each submitted change adds a
commit and at least one data part. Two commands shrink this.

```
SQUASH COMMITS [AFTER "<hash>"] [THROUGH "<hash>"]
MERGE_DATAPARTS
```

## SQUASH COMMITS

Folds the commits of the range into its last commit. Without `AFTER` the range starts at the
first commit; without `THROUGH` it ends at the head. Entity IDs do not change.

It reduces the number of commits, not the number of data parts. The squashed commit still
reads every data part of the range, so a graph with 386 data parts has 386 after the squash.
Only two kinds of data part directory are deleted:

- directories no listed commit references, such as leftovers of a dump that failed;
- the data parts a `MERGE_DATAPARTS` commit inside the range replaced.

Before it squashes, it writes every commit in memory that is not yet on disk. If that write
fails, for example because the disk is full, the squash is refused and nothing changes in
memory or on disk.

An open change on a commit inside the range, other than its last one, blocks the squash.

## MERGE_DATAPARTS

Adds a commit on the head whose single data part replaces all data parts of the graph. This is
the command that reduces the number of data parts. Run `SQUASH COMMITS` after it to drop the
commits, and with them the data parts, that the merge made unreachable.

An open change on the graph being merged blocks the merge. Open changes on other graphs do not.
