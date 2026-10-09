# Pinned Tree-sitter grammars

`manifest.json` pins official upstream release commits, SHA256 hashes of their
GitHub codeload archives, and hashes of their C sources, headers, and licenses.
Generated sources remain under `.tools/grammars`; they are not vendored into Git.

Run `./scripts/prepare-grammars.ps1` before configuring a clean Windows checkout.
The script verifies both archives and extracted compilation inputs. It never runs
grammar repository scripts or downloads a grammar generator.

Include `xmake/grammars.lua` from the root build. The `astra-language` target needs
`astra-core`, the `tree-sitter` package, and `astra-grammar-c`, `astra-grammar-cpp`,
`astra-grammar-javascript`, `astra-grammar-typescript`, `astra-grammar-tsx`,
`astra-grammar-json`, `astra-grammar-python`, `astra-grammar-markdown`, and
`astra-grammar-markdown-inline`. Each grammar compiles with its own generated
`tree_sitter/parser.h`, so ABI versions are not mixed between generated sources.

The runtime adapter checks grammar ABI compatibility when creating a parser.
Grammar licenses remain in their verified source directories. Distribution must
include those licenses alongside the Tree-sitter runtime license.

Syntax highlighting uses actual concrete syntax node types and context. It is
syntactic coloring, not LSP semantic-token classification. Markdown uses both
block and inline grammars; embedded fenced-code language injection remains a
separate layer. The worker returns revision-tagged immutable results, and the
service must discard a result when `matches(current_snapshot)` is false.
