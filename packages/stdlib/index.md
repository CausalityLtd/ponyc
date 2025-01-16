# Standard Library

Contains all packages available within the standard library.

## Contributions

If you wish to contribute to improving this documentation, please open pull requests on the [ponyc](https://github.com/ponylang/ponyc) repository after reading through our [contributing](https://github.com/ponylang/ponyc/blob/main/CONTRIBUTING.md) guidelines.

### Building Documentation Locally

Pony has a documentation generator which builds into mkdocs format; we then use `mkdocs` to build and deploy the documentation. In brief, these are the steps to build a local instance of the `stdlib` documentation.

```console
ponyc packages/stdlib --docs-public --pass expr
sed -i 's/site_name:\ stdlib/site_name:\ Pony Standard Library/' stdlib-docs/mkdocs.yml
sed -i -e '1 e cat packages/stdlib/index.md' stdlib-docs/docs/index.md
cd stdlib-docs; mkdocs serve; cd ../
```
