# supervisor

A small process supervisor, written to show one thing: a contract's lifecycle
read as the lifecycle of a real process. `service.ash` declares a `Service`
contract whose four states are the four states of a service, and one contract
instance is one run of that service. The Python in `supervisor.py` is the host.
It spawns processes, watches them, and drives each instance through its states
with the same five calls every Ashford host uses.

## The state machine

The contract state is the service state, with no translation layer in between.

| contract state | service state | how it is reached |
| -------------- | ------------- | ----------------- |
| Signed         | starting      | the process is spawned, not yet healthy |
| Partial        | running       | `start` and `ready` both landed, `finish` pending |
| Fulfilled      | clean exit    | the run finished on an `Ok`, all three latched |
| Broken         | crashed       | a latch broke before the clean finish |

The `requirements` block in `service.ash` is where this is written down, and it
is the whole of it. `start` and `ready` are abstract: the host spawns the
process and runs the first health pass, then binds the two pledges to what it
saw. `finish` and `crashes` are in the language. `finish` writes the run's row
and latches the state, returning `Err(code)` on an unclean exit so the pledge
latches broken and the contract breaks itself. `crashes` counts the unclean
runs already on record.

## park and resume

The supervisor can restart without losing the services it watches. On `SIGTERM`
it parks every running instance into a park store keyed by the service name, one
`park` call per instance, and exits. The processes themselves survive because
each is spawned in its own session with `setsid`, so it is not in the
supervisor's process group and does not take the signal. Started again with
`--resume`, the supervisor stands each parked instance back up from its key,
reads the recorded pid back from the pidfile, and reattaches. A running service
never notices its supervisor cycled.

Parking is a write, not an ending. A parked instance is still Partial, still
running, and the row in the park store is the state between two supervisor
lifetimes, never a snapshot of one mid fulfillment.

## The store

Every run writes one row to the `Runs` table, and the table is shared across
every instance that binds the same `dsn`. The `service` column carries the
instance's name, so runs of different services sit in one table without
colliding. `crashes()` reads it back with a single predicate query, counting
the rows whose service is this instance's name and whose `clean` column is
false. That count is the supervisor's restart evidence: a service past its
unclean run limit is left down instead of spawned again. The count survives a
supervisor restart because it lives in the database, not in the host.

## Running it

The one line:

```text
make test-supervisor
```

By hand, build the contract first and then drive it with the host:

```text
ashc build examples/supervisor/service.ash
python examples/supervisor/supervisor.py \
    --dsn file:target/supervisor.db \
    --piddir target/supervisor-pids \
    --service web="sleep 60" \
    --service worker="sleep 60"
```

Each `--service NAME=CMD` signs one `Service` instance with `name` and `cmd`
set, spawns `CMD`, and walks the instance. `--dsn` is the database the `Runs`
table and the park store both live in, and `--piddir` is where the pidfiles are
written for resume to find.

## Watching it

With `--grpc PORT` the supervisor serves a small read only observer surface,
its own two rpcs and deliberately not the emitted contract surface: an
observer must not be able to sign, fulfill, or break anything, so the
contract's bridge stays unserved and the host serves its own. What crosses is
the diagnosis the contract already keeps: the state name, the partial name
lists, and the crash count off the `Runs` table.

```text
python examples/supervisor/watch.py --port 50259 list
web Partial pid 4242 run 17 crashes 0

python examples/supervisor/watch.py --port 50259 get web
web Partial pid 4242 run 17 crashes 0
fulfilled: start,ready
pending: finish,crashes
broken:
```

`make test-supervisor-watch` gates it: two services up, one already through a
crash and a restart, the list and the detail read from outside the process,
and an unknown name answered NOT_FOUND. It skips clean without grpcio, like
the bridge gates.

## What this does not do

This is a development supervisor, one rung up from a shell loop, not an init
system. It is honest about the rung:

- No cgroups, no resource limits, no namespaces. A supervised process is an
  ordinary child, not a contained one.
- No dependencies between services. Each instance is independent, and nothing
  orders one service's start against another's.
- No socket activation and no readiness beyond the one health pass the host
  binds to `ready`.
- Not PID 1. It does not reap orphans it did not spawn, and it is not built to
  be a container entrypoint.

What it does show is the seam: a host drives a store backed, parkable contract
with the five calls it always had, and the service lifecycle falls out of the
contract lifecycle for free.
