// The supervisor's Service contract: the lifecycle read as a process, one
// contract instance to one run of the service. The four states carry their
// operational meaning directly, and the requirements block below is the only
// place that meaning is written down.
//
//   Signed     the service is starting, spawned but not yet healthy
//   Partial    the service is running, start and ready both landed
//   Fulfilled  the service exited clean, its run finished on an Ok
//   Broken     the service crashed, a latch broke before the clean finish
//
// start and ready are abstract on purpose. The host spawns the process and
// runs the first health pass, then binds these two pledges to what it saw, an
// Ok(pid) or an Ok(true) or an Err with the errno. finish and crashes are in
// the language: finish records the run and latches the state, and crashes
// counts the unclean runs already on record, the restart limit's evidence.

import ashstd.store

contract Service {
    vow name: String
    vow cmd: String
    vow dsn: String

    // One row per run of one service, shared across every instance that binds
    // the same dsn. The host issues a unique run id, the service column carries
    // this instance's name, and clean records whether the run exited on its own
    // terms. crashes reads this table back across restarts.
    schema Runs {
        id: Int          // pk, host issues unique run ids
        service: String
        clean: Bool
    }

    // The host spawns the process and answers with the pid on success or the
    // errno on failure. Abstract, bound by the host once the spawn returns.
    pledge start() -> Result<Int, Int>

    // The host's first health pass, an Ok(true) once the process answers.
    // Abstract, bound by the host once the probe returns.
    pledge ready() -> Result<Bool, Int>

    // Records the end of a run and latches the state with it. The row is the
    // Runs record for this run, its service column the instance's own name read
    // straight from the vow, and a store that refuses the insert is Err(75), the
    // one failure this pledge folds to a status. A run that did not exit clean
    // returns Err(code) so the pledge latches broken and the contract breaks
    // itself; a clean run returns Ok and the pledge latches fulfilled.
    pledge finish(run: Int, code: Int, clean: Bool) -> Result<Unit, Int> {
        let row = Runs { id: run, service: name, clean: clean }
        let landed = match Store.insert(Runs, row) {
            Ok(done) -> done
            _ -> { return Err(75) }
        }
        if !clean {
            return Err(code)
        }
        return Ok(landed)
    }

    // The number of unclean runs this service has on record, counted with one
    // predicate read: every Runs row whose service is this instance's name and
    // whose clean column is false. The name comes straight from the vow and the
    // comparison lowers onto the prepared statement, so the count is the store's
    // answer, not a scan in the pledge. A store failure is Err(75); no unclean
    // run is a clean Ok(0). The supervisor reads this to decide whether a
    // service has crashed too many times to restart.
    pledge crashes() -> Result<Int, Int> {
        return match Store.query(Runs, service == name && clean == false) {
            Ok(rows) -> {
                let mut n = 0
                for row in rows {
                    n = n + 1
                }
                Ok(n)
            }
            _ -> Err(75)
        }
    }

    // The state machine, written as policy. Fulfilled is the clean exit, start
    // and ready and finish all latched. Partial is the running service, start
    // and ready landed with finish still pending. break arms only after a latch
    // has broken, so a running or a starting service never trips it; once a
    // latch does break, an unfulfilled finish reads as the crash. fulfill needs
    // finish latched and break needs it unlatched, so the two can never hold at
    // once.
    requirements {
        fulfill: start && ready && finish
        partial: start && ready
        break: !finish
    }
}
