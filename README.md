# E# (E-Sharp)

E# is a small, container-bracketed programming language and reference interpreter written entirely in C. It features a unique lexical structure: parentheses handle declarations and conditional parameters, while square brackets manage all executable blocks, array structures, imports, function calls, and data indexing.

---


---

## Quick Start

### 1. Build the Interpreter
Compile the reference interpreter using any standard C99 compiler:

```sh
cc -std=c99 -Wall -Wextra -pedantic EInterpreter.c -lm -o einterpreter
```

### 2. Run a Program
Pass an .esh source file directly into your compiled binary:

```sh
./einterpreter Example.esh
```

---

## The Bracket Rule

In E#, the spacing and context around brackets dictate meaning. Pay close attention to tight versus spaced brackets:

```esharp
double        /< Tight: Function call execution >/
numbers[index]   /< Tight: Array index collection >/

Func main() [    /< Spaced: Defines a function body >/
    if value > 0 [   /< Spaced: Defines a conditional body >/
    loop (count) [   /< Spaced: Defines a loop body >/
```

Note: Parentheses () are strictly reserved for parameter lists, mathematical operations, and loop counters. Never use parentheses to call a function: double(5) is invalid execution syntax.

---

## Syntax Reference

### Comments
```esharp
/< An inline or single-line comment annotation >/

/[ 
   A block documentation comment. 
   It can freely span multiple lines. 
]/
```

### Variables and Core Types
Variables are dynamically typed and declared using the Var keyword:
```esharp
Var score = 10
Var message = "Hello" + " World!"  /< Strings use double quotes and '+' to concatenate >/
Var condition = true               /< Booleans accept true / false >/
```

### Arrays
Arrays are zero-indexed, mutable structures:
```esharp
Var numbers = [4, 2, 1, 3]
Var first = numbers[0]
numbers[1] = 9

Var len = numbers.length         /< Get array size >/
numbers.append[10]               /< Mutates original array >/
Var chunk = numbers.slice[1, 3]  /< Inclusive start, exclusive end >/
```

### Conditionals and Loops
```esharp
if value < 0 [
    return "negative"
] ELSE value == 0 [             /< 'ELSE' handles else-if logic >/
    return "zero"
] ELSE [
    return "positive"
]

loop (5) [                       /< Runs a strict number of iterations >/
    Studio.print["iteration"]
]
```

---

## Standard and External Modules

Before using any module utilities, they must be explicitly imported.

### Built-in Libraries
* Studio: Handles text outputs and pipeline interactions.
  ```esharp
  import [Studio]
  Studio.print["Output:", 42, true]
  Var inputData = INPUT[] /< Globally accessible stdin line reader >/
  ```
* math: Exposes fundamental math operations.
  ```esharp
  import [math]
  Var Root = math.sqrt[81] /< Supports pow, abs, floor, ceil, max, min, PI >/
  ```

### Header Sharp Manifests (.hsh)
You can break code into modular public layers using paired layout systems:
1. Stats.hsh (The Public Interface Manifest)
2. Stats.esh (The Internal E# Execution Logic)

An .hsh manifest can even bind scripts to external command-line utilities via the external keyword:
```text
module Greeting
export greet
external greet python3 greet.py
```

---

## Python Library Integration

The directory containing esharp_lib exposes a clean Python API whose core calculations are delegated straight to E# implementations behind the scenes via a line-based stream protocol.

Ensure einterpreter is built, then execute commands natively from Python:
```sh
PYTHONPATH=. python3 - <<'PY'
import esharp_lib
print(esharp_lib.sort([9, 2, 7, 1, 5]))
print(esharp_lib.fibonacci(8))
PY
```

---

## Editor Customization
To set up development support in VS Code:
1. Open the Extensions View in Visual Studio Code.
2. Install the local pre-packaged VSIX bundle located inside the esharp-language/ directory.
3. This grants syntax validation, structural bracket coloring, and auto-closing configurations for .esh files.

---

## Testing

Verify all built-in functions, returns, arrays, mutation rules, string conversions, and loops by running the single comprehensive test suite:

```sh
./einterpreter test.esh
```
