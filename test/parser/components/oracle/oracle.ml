(* Differential-testing oracle for staticmon's standalone C++ parser.
 *
 * For each formula file given on the command line, parses it with
 * libmonpoly's Formula_parser (jshs/monpoly) and prints exactly one line:
 *
 *   (ok <canonical s-expression of the AST>)
 *   (parse_error "<exception text, for debugging only>")
 *
 * The C++ parser prints the same format; the harness compares lines
 * (for parse_error lines only the head symbol is compared).
 *
 * Each file is parsed in a forked child process because the parser's `_n`
 * fresh-variable counter (Formula_parser.var_cnt) is global mutable state:
 * forking makes every file see the counter at 0, matching monpoly's
 * one-formula-per-process behaviour.
 *)

open Libmonpoly

let buf = Buffer.create 4096
let add = Buffer.add_string buf

let str s =
  add "\"";
  add (String.escaped s);
  add "\""

let app name args =
  add "(";
  add name;
  List.iter (fun arg -> add " "; arg ()) args;
  add ")"

let z (n : Z.t) () = add (Z.to_string n)

let flt (f : float) () = add (Printf.sprintf "%h" f)

let sexp_cst (c : Predicate.cst) () =
  match c with
  | Int n -> app "Int" [ z n ]
  | Str s -> app "Str" [ fun () -> str s ]
  | Float f -> app "Float" [ flt f ]
  | Regexp (p, _) -> app "Regexp" [ fun () -> str p ]

let rec sexp_term (t : Predicate.term) () =
  let t1 name a = app name [ sexp_term a ] in
  let t2 name a b = app name [ sexp_term a; sexp_term b ] in
  match t with
  | Var v -> app "Var" [ fun () -> str v ]
  | Cst c -> app "Cst" [ sexp_cst c ]
  | F2i a -> t1 "F2i" a
  | I2f a -> t1 "I2f" a
  | I2s a -> t1 "I2s" a
  | S2i a -> t1 "S2i" a
  | F2s a -> t1 "F2s" a
  | S2f a -> t1 "S2f" a
  | DayOfMonth a -> t1 "DayOfMonth" a
  | Month a -> t1 "Month" a
  | Year a -> t1 "Year" a
  | FormatDate a -> t1 "FormatDate" a
  | R2s a -> t1 "R2s" a
  | S2r a -> t1 "S2r" a
  | Plus (a, b) -> t2 "Plus" a b
  | Minus (a, b) -> t2 "Minus" a b
  | UMinus a -> t1 "UMinus" a
  | Mult (a, b) -> t2 "Mult" a b
  | Div (a, b) -> t2 "Div" a b
  | Mod (a, b) -> t2 "Mod" a b

let sexp_bound (b : MFOTL.bound) () =
  match b with
  | OBnd n -> app "OBnd" [ z n ]
  | CBnd n -> app "CBnd" [ z n ]
  | Inf -> add "Inf"

let sexp_interval ((l, r) : MFOTL.interval) () =
  app "Interval" [ sexp_bound l; sexp_bound r ]

let sexp_agg_op (op : MFOTL.agg_op) () =
  add
    (match op with
    | Cnt -> "Cnt"
    | Min -> "Min"
    | Max -> "Max"
    | Sum -> "Sum"
    | Avg -> "Avg"
    | Med -> "Med")

let sexp_var_list vs () =
  add "(";
  List.iteri
    (fun i v ->
      if i > 0 then add " ";
      str v)
    vs;
  add ")"

let sexp_pred ((name, arity, args) : Predicate.predicate) () =
  app "Pred"
    [ (fun () -> str name);
      (fun () -> add (string_of_int arity));
      (fun () ->
        add "(";
        List.iteri
          (fun i t ->
            if i > 0 then add " ";
            sexp_term t ())
          args;
        add ")")
    ]

let rec sexp_formula (f : MFOTL.formula) () =
  let f1 name a = app name [ sexp_formula a ] in
  let f2 name a b = app name [ sexp_formula a; sexp_formula b ] in
  let tt name a b = app name [ sexp_term a; sexp_term b ] in
  let unop name i a = app name [ sexp_interval i; sexp_formula a ] in
  let binop name i a b =
    app name [ sexp_interval i; sexp_formula a; sexp_formula b ]
  in
  let letop name p a b =
    app name [ sexp_pred p; sexp_formula a; sexp_formula b ]
  in
  match f with
  | Equal (a, b) -> tt "Equal" a b
  | Less (a, b) -> tt "Less" a b
  | LessEq (a, b) -> tt "LessEq" a b
  | Substring (a, b) -> tt "Substring" a b
  | Matches (a, b, opts) ->
      app "Matches"
        [ sexp_term a;
          sexp_term b;
          (fun () ->
            add "(";
            List.iteri
              (fun i o ->
                if i > 0 then add " ";
                match o with
                | None -> add "None"
                | Some t -> app "Some" [ sexp_term t ])
              opts;
            add ")")
        ]
  | Pred p -> sexp_pred p ()
  | Let (p, a, b) -> letop "Let" p a b
  | LetPast (p, a, b) -> letop "LetPast" p a b
  | Frz (p, a, b) -> letop "Frz" p a b
  | Neg a -> f1 "Neg" a
  | And (a, b) -> f2 "And" a b
  | Or (a, b) -> f2 "Or" a b
  | Implies (a, b) -> f2 "Implies" a b
  | Equiv (a, b) -> f2 "Equiv" a b
  | Exists (vs, a) -> app "Exists" [ sexp_var_list vs; sexp_formula a ]
  | ForAll (vs, a) -> app "ForAll" [ sexp_var_list vs; sexp_formula a ]
  | Aggreg (_tsymb, res, op, agg, gby, a) ->
      (* _tsymb is always TSymb (TAny, 0) fresh from the parser; omitted. *)
      app "Aggreg"
        [ (fun () -> str res);
          sexp_agg_op op;
          (fun () -> str agg);
          sexp_var_list gby;
          sexp_formula a
        ]
  | Prev (i, a) -> unop "Prev" i a
  | Next (i, a) -> unop "Next" i a
  | Eventually (i, a) -> unop "Eventually" i a
  | Once (i, a) -> unop "Once" i a
  | Always (i, a) -> unop "Always" i a
  | PastAlways (i, a) -> unop "PastAlways" i a
  | Since (i, a, b) -> binop "Since" i a b
  | Trigger (i, a, b) -> binop "Trigger" i a b
  | Until (i, a, b) -> binop "Until" i a b
  | Release (i, a, b) -> binop "Release" i a b
  | Frex (i, r) -> app "Frex" [ sexp_interval i; sexp_regex r ]
  | Prex (i, r) -> app "Prex" [ sexp_interval i; sexp_regex r ]

and sexp_regex (r : MFOTL.regex) () =
  match r with
  | Skip n -> app "Skip" [ (fun () -> add (string_of_int n)) ]
  | Test a -> app "Test" [ sexp_formula a ]
  | Concat (a, b) -> app "Concat" [ sexp_regex a; sexp_regex b ]
  | Plus (a, b) -> app "Plus" [ sexp_regex a; sexp_regex b ]
  | Star a -> app "Star" [ sexp_regex a ]

let sexp_tcst (t : Predicate.tcst) () =
  add
    (match t with
    | TInt -> "TInt"
    | TStr -> "TStr"
    | TFloat -> "TFloat"
    | TRegexp -> "TRegexp")

(* Db.schema in returned order: user predicates in reverse declaration order
   (Db.add_predicate prepends), base schema (tp/ts/tpts) last. *)
let sexp_schema (s : Db.schema) () =
  add "(Schema";
  List.iter
    (fun (p, attrs) ->
      add " (";
      str p;
      List.iter
        (fun (v, t) ->
          add " (";
          str v;
          add " ";
          sexp_tcst t ();
          add ")")
        attrs;
      add ")")
    s;
  add ")"

(* Frames starting with the line "#SIG" are signatures (the marker line is a
   comment in both syntaxes, so it cannot collide with real content). *)
let sig_marker = "#SIG\n"

let parse_and_print (src : string) =
  Buffer.clear buf;
  let n = String.length sig_marker in
  (if String.length src >= n && String.sub src 0 n = sig_marker then
     let body = String.sub src n (String.length src - n) in
     match Log_parser.parse_signature body with
     | s ->
         add "(ok ";
         sexp_schema s ();
         add ")"
     | exception e ->
         add "(parse_error ";
         str (Printexc.to_string e);
         add ")"
   else
     match
       Formula_parser.formula Formula_lexer.token (Lexing.from_string src)
     with
    | f ->
        add "(ok ";
        sexp_formula f ();
        add ")"
    | exception e ->
        add "(parse_error ";
        str (Printexc.to_string e);
        add ")");
  print_string (Buffer.contents buf);
  print_newline ()

(* Fork a child per formula: resets Formula_parser.var_cnt for each input,
   matching monpoly's one-formula-per-process behaviour. *)
let run_in_child src =
  match Unix.fork () with
  | 0 ->
      (try parse_and_print src
       with e ->
         print_endline ("(driver_error " ^ Printexc.to_string e ^ ")"));
      exit 0
  | pid -> ignore (Unix.waitpid [] pid)

let read_file fname =
  let ic = open_in_bin fname in
  let n = in_channel_length ic in
  let s = really_input_string ic n in
  close_in ic;
  s

(* stdin protocol (used when no file arguments are given, because docker bind
   mounts may be unavailable): a sequence of frames, each a decimal byte-count
   line followed by exactly that many bytes of formula text. One result line
   is printed per frame. *)
let run_stdin () =
  try
    while true do
      let size_line = input_line stdin in
      let n = int_of_string (String.trim size_line) in
      run_in_child (really_input_string stdin n)
    done
  with End_of_file -> ()

let () =
  if Array.length Sys.argv > 1 then
    for i = 1 to Array.length Sys.argv - 1 do
      run_in_child (read_file Sys.argv.(i))
    done
  else run_stdin ()
