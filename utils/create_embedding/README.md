# create_embedding.py
This script creates a single Agency Copilot-assisted embedding from text, a
file, a folder tree, or stdin.

Usage:
```powershell
python create_embedding.py --path <folder-to-embed>
```

By default, output is a pgvector-compatible vector literal like `[0.1,0.2,...]`.
That same output is also a JSON numeric array, so it can be parsed by other
embedding-aware tools.

## Examples

Create an embedding directly from a plain string and print only the vector to
stdout:

```powershell
python create_embedding.py "Example target in Makefile Called"
```

The Makefile example uses the same plain-string mode:

```powershell
make example
```

If `agency` is not on the WSL path, the script auto-detects the Windows bridge
and runs Agency Copilot through `cmd.exe /c agency copilot`.

Create one embedding from every file under a folder, including subfolders:

```powershell
python create_embedding.py --path .\docs
```

Create an embedding from one file:

```powershell
python create_embedding.py --path .\notes.txt
```

Emit metadata plus the embedding and pgvector literal:

```powershell
python create_embedding.py --path .\docs --format json --pretty
```

Use the default output with PostgreSQL pgvector:

```sql
INSERT INTO documents (content, embedding)
VALUES ('Text to embed', '[0.1,0.2,...]'::vector);
```

Folder input is concatenated into one payload with file-path delimiters before
Agency Copilot processes it. Copilot context-size limits still apply.

Use the same `--dimensions` value when creating vectors that you plan to compare
with each other. The default is 1536 dimensions.
