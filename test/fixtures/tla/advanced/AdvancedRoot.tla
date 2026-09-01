---- MODULE AdvancedRoot ----
EXTENDS
  AdvA,
  AdvB

Text == "EXTENDS FakeString INSTANCE FakeString2"
(* outer comment
   EXTENDS FakeComment
   (* INSTANCE FakeNested *)
*)
\* INSTANCE FakeLine
Op == INSTANCE AdvC WITH x <- 1
====
