# Bundled language tools

Astra resolves the default server names from its explicit resource directory.
For a packaged app, use `tools/` beside the executable. In this checkout, pass
the repository as `resource_directory` to use `.tools/lsp/tools/` and the pinned
clangd cache. An unknown bare executable name is rejected; custom servers use
an explicit executable path and separate arguments, without a shell.

Run from PowerShell 7 on Windows x64:

```powershell
./scripts/prepare-language-tools.ps1 -IncludeClangd
./scripts/prepare-language-tools.ps1 -IncludeClangd -PackageDirectory build/package
```

Close any Astra process using these tools before replacing its runtime cache.
The scripts only prepare Windows assets. Other platforms have not been run or
validated in this environment.

`manifest.json` pins Node.js 24.21.0, TypeScript 6.0.3,
typescript-language-server 6.0.1 and basedpyright 1.40.2. The preparation script
checks SHA-256 for every archive and the npm registry SHA-512 integrity for each
package. All three packages bundle their runtime dependencies; package metadata
is checked to reject additional unpinned dependencies. No npm executable,
installation scripts, user npmrc, global package installation or user package
cache is used. Downloads, staging and extracted resources stay in this repository.
The manifest also pins the relative paths and SHA-256 hashes of the Node, LLVM,
TypeScript, TypeScript Language Server, basedpyright and bundled typeshed license
or notice files. Preparation and release copying both verify those files.

The Node runtime retains its upstream `LICENSE` with third-party notices.
TypeScript and typescript-language-server retain their Apache-2.0 license files;
basedpyright retains its MIT license and bundled typeshed/stub notices. Complete
package trees, including notices, are copied to a release package. clangd 21.1.8
is separately pinned in `scripts/prepare-clangd.ps1`, which copies its complete
LLVM resource tree and licenses, using the same manifest.

TypeScript Language Server is launched with the bundled `tsserver.js` selected
explicitly. Automatic typing acquisition is disabled, so it will not invoke npm
or download external type packages. The client advertises plain-text completion;
snippet-only items are rejected explicitly instead of inserting placeholders.
basedpyright supplies Python semantic intelligence; it does not advertise a
formatting provider. Requests unsupported by the selected server preserve its
JSON-RPC error rather than claiming success.

Official sources:

- [Node release and checksums](https://nodejs.org/dist/v24.21.0/)
- [TypeScript 6 package](https://registry.npmjs.org/typescript/6.0.3)
- [TypeScript Language Server package](https://registry.npmjs.org/typescript-language-server/6.0.1)
- [TypeScript Language Server setup](https://github.com/typescript-language-server/typescript-language-server)
- [TypeScript initialization settings](https://github.com/typescript-language-server/typescript-language-server/blob/master/docs/configuration.md)
- [basedpyright package](https://registry.npmjs.org/basedpyright/1.40.2)
- [basedpyright installation](https://docs.basedpyright.com/latest/installation/command-line-and-language-server/)
- [clangd release](https://github.com/clangd/clangd/releases/tag/21.1.8)
