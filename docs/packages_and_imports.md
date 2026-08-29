# Cloth Stage 8.0 packages and imports

Stage 8.0 gives every file type a stable, portable identity derived from its
source path. Source code does not repeat that identity in a module declaration
and does not embed filesystem paths as string literals.

## Project layout

The compiler searches from the entry source toward the filesystem root for
`cloth.toml`. When found, the manifest's directory is the project root and its
`src/` directory is the source root:

```text
project/
  cloth.toml
  src/
    Main.co
    models/
      User.co
```

`src/Main.co` has identity `Main`; `src/models/User.co` has identity
`models.User`. Package directory components and file stems must be valid Cloth
identifiers. Source files must use the `.co` extension.

If no manifest is found, the entry file's directory is the source root and the
command retains standalone behavior. Standalone mode loads command-line files
and their imports but does not automatically compile unrelated sibling files.

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

The compiler starts with the entry sources, discovers their packages and
imports, and repeats until the graph is closed. Explicit imports map directly
to one `.co` file. Wildcard packages are enumerated in logical-name order.
Project packages include all direct sibling `.co` files so same-package public
classes require no import.

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
6. core symbols

An explicit import takes precedence over a wildcard. Two wildcard imports that
provide the same local class name are ambiguous unless an explicit import or
alias resolves that name. Imports are file-scoped and never re-exported.

## Deferred boundary

Stage 8.0 establishes the local project graph. Manifest dependency tables,
package registries, version selection, remote retrieval, and a standard-library
distribution mechanism remain later build-system work; none require a change
to the source import grammar.
