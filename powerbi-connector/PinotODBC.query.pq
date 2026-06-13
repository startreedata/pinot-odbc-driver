// Test queries for the Power Query SDK (Evaluate Current Power Query File).
// Edit host/port to point at a reachable Pinot broker, then run.

let
    db = PinotODBC.Database("localhost", 8000, [Controller = "localhost:9000"])
in
    db

// Other things to try once connected:
//
//   // a folded aggregate (DirectQuery-style)
//   Table.Group(
//       PinotODBC.Database("localhost", 8000, [Controller = "localhost:9000"]){[Name = "baseballStats"]}[Data],
//       {"playerName"}, {{"hr", each List.Sum([homeRuns]), type number}})
//
//   // multi-stage engine (for JOINs)
//   PinotODBC.Database("localhost", 8000, [Controller = "localhost:9000", UseMultistage = true])
