# Cloth packages and imports

Stage 8.0 gives every file type a stable, portable identity derived from its
source path. Source code does not repeat that identity in a module declaration
and does not embed filesystem paths as string literals.

## Source roots

Shuttle reads `Shuttle.toml`, resolves the local package graph, and supplies
each source root explicitly to `clothc` through protocol version 1:

```text
project/
  Shuttle.toml
  src/
    Main.co
    models/
      User.co
```

`src/Main.co` has identity `Main`; `src/models/User.co` has identity
`models.User`. Package directory components and file stems must be valid Cloth
identifiers. Source files must use the `.co` extension.

The compiler never reads the manifest. Direct compiler use supplies
`--source-root=PATH`; without that option, the first entry file's directory is
the standalone root. Standalone mode loads command-line files and their imports
but does not automatically compile unrelated sibling files.

## Import forms

```cloth
import models::User;
import models::User as ModelUser;
import services.api::*;
import RootType;
```

- `.` traverses package directories.
- `::` selects one implicit file type.
- `.*` imports every public `.co` file directly in a package.
- `as` creates a file-local type alias.
- A single identifier selects a class in the root package.

Wildcards do not recurse into child packages and do not import individual
fields or functions. Members remain qualified by their file class, such as
`User.Find(id)`.

## Discovery and ordering

In Shuttle protocol mode, the compiler recursively enumerates every exact `.co`
file beneath the supplied roots without following directory symbolic links.
Direct mode starts with the entry sources and closes their import graph.
Explicit imports map to one `.co` file; wildcard packages are enumerated in
logical-name order. Direct projects with an explicit source root include direct
same-package siblings so public classes require no import.

The completed graph is sorted by qualified file-type identity before semantic
handles and ABI names are allocated. Canonical paths are deduplicated. Sources
outside the source root, invalid package components, case-only identity
collisions, and unresolved imports are errors.

Import cycles are valid. Imports do not execute code and are not textual
includes; the existing two-pass architecture registers all file types and
member signatures before checking executable bodies.

## Visibility and ambiguity

Capitalization retains its existing meaning. Wildcards expose only public file
classes. A private file class remains usable only within its own source file,
even when another file shares its package.

Resolution order is:

1. lexical locals and parameters
2. current file-class members
3. current-package public file classes
4. explicit imports and aliases
5. wildcard imports
6. public file types beneath `cloth.lang`
7. core symbols

An explicit import takes precedence over a wildcard. Two wildcard imports that
provide the same local class name are ambiguous unless an explicit import or
alias resolves that name. Imports are file-scoped and never re-exported.

Enums participate in the same type lookup and import rules as classes and
interfaces. Every case of an accessible enum is public regardless of spelling.
An alias changes the lookup name, not nominal identity or printed names;
wildcards import types, never bare cases. Source-free artifact consumers retain
the complete ordered case set. See [enums](enums.md).

## Standard-library prelude

When the canonical `cloth` package is part of the compilation, every public
file type beneath `cloth.lang` is available by its short name without an
import. The compiler derives this fallback from the same verified source or
package artifact supplied for normal imports; it does not scan for library
files or synthesize an AST import.

Prelude lookup is type-only and recursive across that namespace tree. Members
remain qualified by their file type and private files do not participate.
Public short names must be unique across the tree; a duplicate invalidates the
standard library with a deterministic diagnostic. A higher-priority same-
package or explicitly imported name wins silently. The canonical prelude type
remains reachable through an alias such as:

```cloth
import cloth.lang.errors::ArgumentError as StandardArgumentError;
```

An explicit wildcard import of a concrete `cloth.lang` package remains ordinary
and therefore retains ordinary collision diagnostics. Without a canonical
`cloth` source or artifact input, an unqualified library type is simply unknown.
The initial prelude API is `ArgumentError` and `StateError`, both ordinary
extensible error types with default and message constructors.

## Shuttle dependency boundary

A Shuttle dependency alias becomes the first component of an import. Given a
direct dependency alias `models`, `models::User` selects `User.co` at that
dependency root and `models.data::Record` selects `data/Record.co`. Only direct
dependencies are visible, and aliases cannot collide with a local top-level
source package.

Package names, versions, graph cycles, and path dependencies are Shuttle build
concerns. Remote retrieval, version solving, and registries remain deferred.
The approved [Stage 35 standard-library contract](proposals/stage_35_standard_library_foundation.md)
reserves `cloth` within this same identifier grammar. Shuttle automatically
injects the exact library paired with the selected compiler as the direct
`cloth` dependency of every ordinary package. Users do not declare that edge,
and types such as `cloth.math::Math` still require explicit imports. See
[Shuttle and the Cloth compiler](shuttle_and_compiler.md).
