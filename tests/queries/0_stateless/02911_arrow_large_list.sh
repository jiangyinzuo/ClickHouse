#!/usr/bin/env bash
# Tags: no-fasttest
CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

# ## generate arrow file with python
# import pyarrow as pa
# schema = pa.schema([ pa.field('a', pa.large_list(pa.utf8())) ])
# a = pa.array([["00000", "00001", "00002"], ["10000", "10001", "10002"]])
# with pa.OSFile('arraydata.arrow', 'wb') as sink:
#    with pa.ipc.new_file(sink, schema=schema) as writer:
#        batch = pa.record_batch([a], schema=schema)
#        writer.write(batch)

# cat arraydata.arrow | base64

cat <<EOF | base64 --decode |  $CLICKHOUSE_LOCAL --query='SELECT * FROM table FORMAT TSVWithNamesAndTypes' --input-format=Arrow
QVJST1cxAAD/////mAAAABAAAAAAAAoADAAGAAUACAAKAAAAAAEEAAwAAAAIAAgAAAAEAAgAAAAE
AAAAAQAAAAQAAADY////AAABFRQAAAAYAAAABAAAAAEAAAAgAAAAAQAAAGEAAADI////EAAUAAgA
BgAHAAwAAAAQABAAAAAAAAEFEAAAABwAAAAEAAAAAAAAAAQAAABpdGVtAAAAAAQABAAEAAAA////
/8gAAAAUAAAAAAAAAAwAFgAGAAUACAAMAAwAAAAAAwQAGAAAAFgAAAAAAAAAAAAKABgADAAEAAgA
CgAAAGwAAAAQAAAAAgAAAAAAAAAAAAAABQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABgAAAAA
AAAAGAAAAAAAAAAAAAAAAAAAABgAAAAAAAAAHAAAAAAAAAA4AAAAAAAAAB4AAAAAAAAAAAAAAAIA
AAACAAAAAAAAAAAAAAAAAAAABgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwAAAAAAAAAGAAAAAAAA
AAAAAAAFAAAACgAAAA8AAAAUAAAAGQAAAB4AAAAAAAAAMDAwMDAwMDAwMTAwMDAyMTAwMDAxMDAw
MTEwMDAyAAD/////AAAAABAAAAAMABQABgAIAAwAEAAMAAAAAAAEADwAAAAoAAAABAAAAAEAAACo
AAAAAAAAANAAAAAAAAAAWAAAAAAAAAAAAAAAAAAAAAAAAAAIAAgAAAAEAAgAAAAEAAAAAQAAAAQA
AADY////AAABFRQAAAAYAAAABAAAAAEAAAAgAAAAAQAAAGEAAADI////EAAUAAgABgAHAAwAAAAQ
ABAAAAAAAAEFEAAAABwAAAAEAAAAAAAAAAQAAABpdGVtAAAAAAQABAAEAAAAyAAAAEFSUk9XMQ==
EOF
