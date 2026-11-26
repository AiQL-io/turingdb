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

## Background compaction

The server runs both commands on every loaded graph, once an hour by default:

- commits created more than 7 days ago are squashed into one, as `SQUASH COMMITS THROUGH`
  the newest of them would. A commit inside the window is never squashed, so neither is any
  commit after it;
- a head commit that reads more than 16 data parts is merged, as `MERGE_DATAPARTS` would.

A graph whose open change blocks either command is left as it is until the next run.

```
turingdb start -history-retention-hours 168     # 0 keeps all history
turingdb start -compaction-interval-minutes 60  # 0 turns the background compaction off
```

Each commit stores the time it was created at the end of its `metadata` file. A commit
written before that field existed reads as created at the epoch, so the first run after an
upgrade squashes it with the other old commits. Older servers stop reading `metadata` before
that field and still load the graph.

## Disk use

Graph files are written in pages of 1 MiB. The unused tail of a page is left unwritten, so a
filesystem that supports holes (ext4, XFS) stores only the bytes in use. A file the server
wrote before this kept its zero tails on disk; `fallocate --dig-holes <file>` frees them in
place without changing the file's contents.
