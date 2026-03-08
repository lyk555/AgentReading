# AgentReading

`AgentReading` is a UE5 plugin for building and querying a local RAG knowledge base around project Lua APIs and Markdown documentation.

It has two modules:

- `AgentReading` (`Runtime`): loads prebuilt RAG index files and retrieves grounded context for a natural-language query.
- `AgentReadingEditor` (`Editor`): provides commandlets to scan source code, chunk docs, align docs to APIs, and test retrieval during development.

## What It Does

The plugin is designed for projects where Lua-exposed APIs are large enough that prompt-only generation is unreliable.

The workflow is:

1. Scan C++ source to build an API registry.
2. Scan Markdown docs to build chunked document records.
3. Build embeddings for those chunks offline.
4. Load the generated files at runtime.
5. Retrieve relevant API entries and document snippets for a user query.

The runtime side does not build embeddings on the fly. It consumes files that were prepared ahead of time.

## Current Architecture Boundary

The current implementation is a single-index RAG design.

- Runtime retrieval reads from `Saved/AgentReading/`
- Expected files are:
  - `ApiRegistry.json` or `EnrichedRegistry.json`
  - `DocChunks.jsonl`
  - `DocEmbeddings.jsonl`

Important limitation:

- You can choose the document source folder and file filters during offline index building.
- You cannot currently switch between multiple document libraries or index roots at runtime through plugin settings.

## Plugin Requirements

- Unreal Engine 5
- `NNERuntimeORT` plugin enabled for local ONNX embedding mode
- Python environment for offline embedding generation
- A compatible tokenizer and embedding ONNX model if using local runtime embeddings

The plugin descriptor already declares a dependency on `NNERuntimeORT`.

## Settings

Project settings path:

- `Edit -> Project Settings -> Plugins -> Agent Reading`

Config section:

- `[/Script/AgentReading.AgentReadingSettings]`

Available settings:

- `EmbeddingBackend`
  - `Auto`
  - `Python (Dev Only)`
  - `Local ONNX`
- `PythonExeOverride`
- `PythonEmbedScript`
- `PythonModelName`
- `LocalOnnxModelPath`
- `LocalTokenizerPath`
- `LocalRuntimeName`
- `LocalMaxTokens`

Default config in this project currently looks like:

```ini
[/Script/AgentReading.AgentReadingSettings]
EmbeddingBackend=Auto
PythonEmbedScript=Tools/embeddings/embed_query.py
PythonModelName=sentence-transformers/all-MiniLM-L6-v2
LocalOnnxModelPath=Content/AI/RAG/Models/embedding_model.onnx
LocalTokenizerPath=Content/AI/RAG/Models/tokenizer.json
LocalRuntimeName=NNERuntimeORTCpu
LocalMaxTokens=256
```

Notes:

- `Python` backend is mainly for development and validation.
- `Local ONNX` is the intended runtime path for packaged usage.
- Query embeddings and document embeddings must be generated with the same model family and compatible pooling logic.

## Generated Files

By default, the plugin expects or produces the following files under `Saved/AgentReading/`:

- `ApiRegistry.json`
  - Extracted from C++ Lua registration code.
- `DocChunks.jsonl`
  - Chunked Markdown records with metadata and candidate API keys.
- `EnrichedRegistry.json`
  - API registry with doc backreferences attached.
- `DocEmbeddings.jsonl`
  - Embedding vectors for each chunk.
- `AlignmentReport.json`
  - Optional report for doc-to-API alignment.
- `SearchResult.json`
  - Optional output from the query commandlet.

## Offline Index Build Workflow

### 1. Build API Registry

This scans C++ source files for Lua registration patterns.

Example:

```powershell
UnrealEditor-Cmd.exe BuildingGenerator.uproject -run=BuildApiRegistry -Src="F:/UE5/BuildingGenerator/BuildingGenerator/Source" -Out="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/ApiRegistry.json"
```

Current extractor targets patterns like:

- `LUA_ENSURE_GLOBAL_TABLE(...)`
- `REGISTER_LUA_STATIC(...)`

If your project uses a different Lua registration style, this commandlet will need to be extended.

### 2. Build Doc Chunks

This scans a Markdown documentation folder and converts files into JSONL chunks.

Example:

```powershell
UnrealEditor-Cmd.exe BuildingGenerator.uproject -run=BuildDocChunks -Docs="F:/UE5/BuildingGenerator/BuildingGenerator/Doc" -Api="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/ApiRegistry.json" -Out="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/DocChunks.jsonl"
```

Optional filters:

- `-Include="pattern1;pattern2"`
- `-Exclude="pattern1;pattern2"`

Example with filters:

```powershell
UnrealEditor-Cmd.exe BuildingGenerator.uproject -run=BuildDocChunks -Docs="F:/UE5/BuildingGenerator/BuildingGenerator/Doc" -Api="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/ApiRegistry.json" -Include="*API*;*Lua*;*reference*" -Exclude="*Draft*;*Archive*" -Out="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/DocChunks.jsonl"
```

Behavior:

- Recursively scans `.md` and `.markdown`
- Splits text blocks, code fences, and table rows into chunks
- Extracts candidate API keys such as `Env.CreateMap`
- Writes JSONL records with `doc_file`, `chunk_id`, `heading`, `type`, `text`, and `candidate_keys`

If `-Include` is omitted, the current default filter only keeps filenames that look like API/Lua/reference docs.

### 3. Align Docs Back to APIs

This attaches document chunk references back onto API records.

Example:

```powershell
UnrealEditor-Cmd.exe BuildingGenerator.uproject -run=AlignDocsToApi -Api="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/ApiRegistry.json" -Chunks="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/DocChunks.jsonl" -Out="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/EnrichedRegistry.json" -Report="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/AlignmentReport.json"
```

### 4. Build Embeddings

Embeddings are generated offline via Python.

Example:

```powershell
python Tools/embeddings/build_embeddings.py --in_chunks Saved/AgentReading/DocChunks.jsonl --out_emb Saved/AgentReading/DocEmbeddings.jsonl --model sentence-transformers/all-MiniLM-L6-v2
```

Optional arguments:

- `--batch`
- `--provider`
  - `sentence_transformers`
  - `transformers_mean_pool`
- `--max_length`

Example for the local ONNX export pipeline used by this project:

```powershell
python Tools/embeddings/build_embeddings.py --in_chunks Saved/AgentReading/DocChunks.jsonl --out_emb Saved/AgentReading/DocEmbeddings.jsonl --model sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2 --provider transformers_mean_pool --max_length 256
```

## Runtime Usage

The main runtime entry point is:

```cpp
FAgentRagContext Context;
FString Error;
const bool bOk = FAgentRagRetriever::BuildContext(
    TEXT("How do I call Env.CreateMap?"),
    8,
    8,
    Context,
    Error);
```

If retrieval succeeds:

- `Context.DocHits` contains matched document chunks
- `Context.ApiHits` contains matched API records
- `Context.BuildPromptSection()` formats the result into a prompt-friendly block

Typical usage pattern:

1. Receive a natural-language user request.
2. Call `FAgentRagRetriever::BuildContext(...)`.
3. Inject `Context.BuildPromptSection()` into your system or tool prompt.
4. Generate Lua or agent output using only retrieved, project-grounded APIs.

## Retrieval Behavior

The retriever combines:

- lexical term matching
- explicit API key matching such as `Table.Function`
- vector similarity over `DocEmbeddings.jsonl`
- API/doc cross-link boosting from aligned candidate keys

This gives better results than plain keyword search while staying fully local.

## Development Query Commandlet

For quick validation during development, use `QueryAgent`.

Example:

```powershell
UnrealEditor-Cmd.exe BuildingGenerator.uproject -run=QueryAgent -Query="How do I use Env.CreateMap?" -TopKDocs=8 -Out="F:/UE5/BuildingGenerator/BuildingGenerator/Saved/AgentReading/SearchResult.json"
```

This commandlet is useful for checking whether your built index is coherent before wiring the plugin into gameplay or editor workflows.

## Repository Layout

Key locations in this plugin/project:

- `Plugins/AgentReading/Source/AgentReading/`
  - runtime code
- `Plugins/AgentReading/Source/AgentReadingEditor/`
  - commandlets and editor-side build tools
- `Tools/embeddings/`
  - offline embedding scripts
- `Content/AI/RAG/Models/`
  - local ONNX model and tokenizer assets
- `Saved/AgentReading/`
  - generated index files

## Known Limitations

- Runtime currently assumes a single index root: `Saved/AgentReading/`
- No built-in runtime selector for multiple knowledge libraries
- API extraction is regex-based and depends on current Lua registration patterns
- Document ingestion currently targets Markdown files only
- Packaged runtime quality depends on the local ONNX model matching the offline embedding pipeline

