# Serialize Check

Checks for object serialization change in FoundationDB.

## Dependencies

To compile `source_scanner`, `Boost::JSON` and LLVM `libtooling` are required.

Typical commands to build `source_scanner` is:

```bash
CC=clang CXX=clang++ LD=lld BOOST_ROOT=/opt/boost_1_78_0 cmake .. -GNinja
ninja
```

To run `renormalize.py`, a compilation database generated by CMake or other build systems is required.

## Usage

Typical usage is to copy both `source_scanner` and `renormalize.py` to the root directory of FoundationDB, and run

```bash
# Generate the compilation database, note that compile_commands.json may not be generated if the build directory is not empty.
# NOTE: OPEN_FOR_IDE is required to prevent the ACTOR compiler transpiles ACTORs.
CC=clang CXX=clang++ LD=lld CMAKE_EXPORT_COMPILE_COMMANDS=1 cmake . -B preconfigure/ -DOPEN_FOR_IDE=ON
./renormalize.py --compilation-database preconfigure/compile_commands.json
```

`source_scanner` may not be able to find certain header files, e.g. `stdarg.h`. It might be necessary to explicitly introduce include path:

```bash
./renormalize.py --compilation-database preconfigure/compile_commands.json --extra-arg="-I/usr/local/lib/clang/15.0.6/include/"
```

### `source_scanner`
`source_scanner` parses a C++ source file, checks for all classes that supports `serialize` method, and translates the information into JSON format for further investigation.

```bash
./source_scanner [-p compilation_database] path_to_source_code
```

### `renormalize.py`
`renormalize.py` runs the `source_scanner` over files in `compile_commands.json`, collects the output, de-duplicates the results and generates a report of serializable classes. It depends on the compilation database generated by CMake or other build systems. A `SerializedObjects.md` will be generated as the artifact. Comparing different versions of `SerializedObjects.md` via `vimdiff` or other diff tool. Running `renormalize.py` over the FoundationDB source code repository may take long time, up to hours.

## Limitations

The checker will not be able to locate any packages, e.g. RocksDB, that are downloaded at the build stage.

For any class member variables that has undetermined types, `int` will be used.

## Sample output

A slice of `SerializedObjects.md` will look like

```markdown
## `fdbserver/include/fdbserver/SpanContextMessage.h`
### `SpanContextMessage`
#### Member variables
0. `spanContext`: `SpanID`
#### Serialize code
\`\`\`c++
	{
		uint8_t poly = MutationRef::Reserved_For_SpanContextMessage;
		serializer(ar, poly, spanContext);
	}
\`\`\`
```
