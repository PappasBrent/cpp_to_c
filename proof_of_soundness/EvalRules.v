Require Import Coq.ZArith.ZArith.
Require Import Coq.Lists.List.

From Cpp2C Require Import Syntax.
From Cpp2C Require Import ConfigVars.

Section EvalRules.

Open Scope Z_scope.


(* Right now, a term that fails to evaluate will simply get "stuck";
   i.e. it will fail to be reduced further.
   We do not provide any error messages, but I think we could add this
   later using a sum type. *)
Reserved Notation
  "[ S , E , G , F , M '|-' e '=>' v , S' ]"
  (at level 90, left associativity).
Reserved Notation
  "{ S , E , G , F , M '=[' s ']=>' S' }"
  (at level 91, left associativity).
Inductive exprevalR :
  state -> environment -> environment -> func_definitions -> macro_definitions ->
  expr ->
  Z -> state -> Prop :=
  (* Numerals evaluate to their integer representation and do not
     change the state *)
  | E_Num : forall S E G F M z,
    [S, E, G, F, M |- (Num z) => z, S]
  (* Variable lookup returns the variable's R-value
     and does not change the state *)
  | E_X_Local : forall S E G F M x l v,
    lookupE x E = Some l ->
    lookupS l S = Some v ->
    [S, E, G, F, M |- (X x) => v, S]
  | E_X_Global : forall S E G F M x l v,
    lookupE x E = None ->
    lookupE x G = Some l ->
    lookupS l S = Some v ->
    [S, E, G, F, M |- (X x) => v, S]
  (* Parenthesized expressions evaluate to themselves *)
  | E_ParenExpr : forall S E G F M e v S',
    [S, E, G, F, M |- e => v, S'] ->
    [S, E, G, F, M |- (ParenExpr e) => v, S']
  (* Unary expressions *)
  | E_UnExpr : forall S E G F M S' uo e v,
    [S, E, G, F, M |- e => v, S'] ->
    [S, E, G, F, M |- (UnExpr uo e) => ((unopToOp uo) v), S']
  (* Binary expressions *)
  (* NOTE: Evaluation rules do not handle operator precedence.
     The parser must use a concrete syntax to generate a parse tree
     with the appropriate precedence levels in it. *)
  | E_BinExpr : forall S E G F M bo e1 e2 S' v1 S'' v2 S''',
    [S, E, G, F, M |- e1 => v1, S'] ->
    [S', E, G, F, M |- e2 => v2, S''] ->
    [S'', E, G, F, M |- (BinExpr bo e1 e2) => (((binopToOp bo) v1 v2)), S''']
  (* Variable assignments update the store by adding a new L-value to
     R-value mapping or by overriding an existing one.
     The R-value is returned along with the updated state *)
  | E_Assign_Success : forall S E G F M l x e v S',
    [S, E, G, F, M |- e => v, S'] ->
    lookupE x E = Some l ->
    [S, E, G, F, M |- (Assign x e) => v, (l,v)::S']
  (* For function calls, each of the function call's arguments are
     evaluated, then the function call itself is evaluated, and finally
     the result of the call is returned along with the ultimate state. *)
  | E_FunctionCall: forall S E G F M x es fstmt fexpr S' v S'',
    definition F x = Some (fstmt, fexpr) ->
    {S, E, G, F, M =[ fstmt ]=> S'} ->
    [S', E, G, F, M |- fexpr => v, S''] ->
    [S, E, G, F, M |- (CallOrInvocation x es) => v, S'']
  (* Macro invocation*)
  (* FIXME *)
  | E_MacroInvocation : forall S E G F M x es mexpr v S',
    invocation M x = Some mexpr ->
    [S, E, G, F, M |- mexpr => v, S'] ->
    [S, E, G, F, M |- (CallOrInvocation x es) => v, S']
  where "[ S , E , G , F , M '|-' e '=>' v , S' ]" := (exprevalR S E G F M e v S')
(* Define the evaluation rule for statements as a
   relation instead of an inductive type to permite the non-
   determinism introduced by while loops *)
with stmtevalR :
  state -> environment -> environment -> func_definitions -> macro_definitions ->
  stmt ->
  state -> Prop :=
  (* A skip statement does not change the state *)
  | E_Skip : forall S E G F M,
    {S, E, G, F, M =[ Skip ]=> S}
  (* An expression statement evaluates its expression and returns 
     the resulting state *)
  | E_ExprStmt : forall S E G F M e v S',
    [S, E, G, F, M |- e => v, S'] ->
    {S, E, G, F, M =[ ExprStmt e ]=> S'}
  (* An empty compound statement evaluates to its initial state *)
  | E_CompoundStatementEmpty : forall S E G F M,
    {S, E, G, F, M =[ CompoundStmt nil ]=> S}
  (* A non-empty compound statement evaluates its first statement and
     then the following statements *)
  | E_CompoundStatementNotEmpty : forall S E G F M stmts s0 rst S' S'',
    head stmts = Some s0 ->
    tail stmts = rst ->
    {S, E, G, F, M =[ s0 ]=> S'} ->
    {S', E, G, F, M =[ CompoundStmt rst ]=> S''} ->
    {S, E, G, F, M =[ CompoundStmt stmts ]=> S''}
  (* An if statement whose condition evaluates to false only carries
     over the side-effects induced by evaluating its condition *)
  | E_IfFalse: forall S E G F M e s0 S',
    [S, E, G, F, M |- e => 0, S'] ->
    {S, E, G, F, M =[ IfStmt e s0 ]=> S'}
  (* An if statement whose condition evaluates to true carries over
     the side-effects from evaluating its condition and its statement *)
  | E_IfTrue: forall S E G F M e s0 v S' S'',
    v <> 0 ->
    [S, E, G, F, M |- e => v, S'] ->
    {S',E, G, F, M =[ s0 ]=> S''} ->
    {S, E, G, F, M =[ IfStmt e s0 ]=> S''}
  (* Side-effects from condition and false branch *)
  | E_IfElseFalse: forall S E G F M e s0 s1 S' S'',
    [S, E, G, F, M |- e => 0, S'] ->
    {S',E, G, F, M =[ s1 ]=> S''} ->
    {S, E, G, F, M =[ IfElseStmt e s0 s1 ]=> S''}
  (* Side-effects from condition and true branch *)
  | E_IfElseTrue: forall S E G F M e s0 s1 v S' S'',
    v <> 0 ->
    [S, E, G, F, M |- e => v, S'] ->
    {S', E, G, F, M =[ s0 ]=> S''} ->
    {S, E, G, F, M =[ IfElseStmt e s0 s1 ]=> S''}
  (* Similar to E_IfFalse *)
  | E_WhileFalse: forall S E G F M e s0 S',
    [S, E, G, F, M |- e => 0, S'] ->
    {S, E, G, F, M =[ WhileStmt e s0 ]=> S'}
  (* A while statement whose condition evaluates to false must be run
     again after evaluating its body *)
  | E_WhileTrue: forall S E G F M e s0 v S' S'' S''',
    v <> 0 ->
    [S, E, G, F, M |- e => v, S'] ->
    {S', E, G, F, M =[ s0 ]=> S''} ->
    {S'', E, G, F, M =[ WhileStmt e s0 ]=> S'''} ->
    {S, E, G, F, M =[ WhileStmt e s0 ]=> S'''}
  where "{ S , E , G , F , M '=[' s ']=>' S' }" := (stmtevalR S E G F M s S').

Close Scope Z_scope.

End EvalRules.