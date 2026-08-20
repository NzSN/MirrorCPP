---- MODULE InstanceMain ----
EXTENDS Integers
INSTANCE InstDep WITH init <- 3

VARIABLE x

Init == x = DepInit
Next == x' = DepNext(x)
====
