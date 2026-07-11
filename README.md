# ashford

Ashford is a statically typed, contract oriented language that sits between other languages. A program declares contracts, other languages sign and fulfill them across the C ABI, and the runtime enforces the terms. No protocol definitions, no serialization boilerplate, no hand written FFI bindings. Any language that can call into a shared C library can talk through an Ashford contract. It also works as a standalone language for writing contract libraries and contract driven programs.

The compiler for Ashford is `ashc`. Source files for Ashford end in `.ash`. The compiler itself is written in a new systems programming language I made called [dusk](https://dusk-lang.org), which emits C that links against a small C runtime. I love programming language design and compilers, and Ashford is the interop half of that obsession: dusk answers how a language should own memory, Ashford answers how two languages should trust each other.

## Core ideas

- Everything is a contract. The contract is the unit of the language, not functions.
- Nothing is implicit. Ownership is always clear, visibility is always stated, and every fallible pledge returns a `Result` or an `Option`.
- Errors are values. No exceptions, and nothing throws across a language boundary.
- Interop is the primary goal of the language. The runtime owns every heap allocation that crosses a boundary, data passes by value in both directions, and a reference is copied on entry and written back on return, never held by Ashford.

## A taste of the language

```text
provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

contract PaymentService {
    incorporate Loggable

    vow currency: String = "USD"

    subcontract Validation {
        pledge validate_card(card: Card) -> Result<Bool, ValidationError>
        pledge validate_amount(amount: Float) -> Result<Bool, ValidationError>
    }

    subcontract Processing {
        pledge charge(card: Card, amount: Float) -> Result<Receipt, PaymentError>
    }

    pledge notify_user(receipt: Receipt) -> Result<Unit, NotifyError>

    requirements {
        fulfill: Validation && Processing && notify_user
        partial: Validation || Processing
        break: !Validation && !Processing && !notify_user
    }

    clause log(message: String) -> Result<Unit, LogError> {
        // log implementation
    }
}
```

A `vow` is an immutable field locked when the contract is signed. A `pledge` is a callable commitment, first class element, with a declared return type. A `clause` is an internal method that never leaves its contract. A `subcontract` groups pledges so a contract can be partially fulfilled, and the `requirements` block writes the fulfillment policy as boolean logic over those groups.

## The lifecycle

A contract moves through five states.

```text
Unsigned -> Signed -> Fulfilled
                   -> PartiallyFulfilled
                   -> Broken
```

`sign()` validates the contract shape, locks the vows, and activates it. Fulfilling a pledge runs it and records the outcome. `break()` tears the contract down and frees every allocation the runtime owns for it. A partially fulfilled contract reports exactly which pledges landed, which are pending, and which broke, with the errors attached.

## How the interop works

The runtime is a shared C library. A compiled `.ash` module carries its contract descriptors, and the runtime registers them in the iname table, a queryable registry of every contract and pledge signature with the language of origin, the type signature hash, and the version baked into each mangled name. A version mismatch is a descriptive error at link time instead of silent corruption at run time. A C program, or anything that can load a C library, looks up a contract, signs it, fulfills pledges, and reads results, all through a handful of runtime calls.

## Status

Design stage. The language surface is being pinned down in [docs/grammar.md](docs/grammar.md) and nothing compiles yet. The build order is the runtime ABI first, then the lexer and parser, then the type checker, then codegen, with a C host driving a real contract end to end as the first milestone that stays green from then on.

## Requirements

Nothing to install yet. When the toolchain lands it will need:

- The dusk toolchain, to build `ashc`.
- clang, to compile the emitted C and the runtime.

## License

TBD.
