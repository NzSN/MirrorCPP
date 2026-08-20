---- MODULE ExtMain ----
EXTENDS Integers, ExtDep

VARIABLE
  \* @type: Int;
  count,
  \* @type: Str;
  action_taken

Init == count = StartValue /\ action_taken = "init"

Next == count' = Inc(count) /\ action_taken' = "tick"

\* Apalache treats the violation as a counterexample = the test trace.
TraceComplete == count < 3
====
