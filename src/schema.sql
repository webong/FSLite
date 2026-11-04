PRAGMA application_id=0x46534c54;
PRAGMA user_version=2;
CREATE TABLE nodes (
    id INTEGER PRIMARY KEY,
    parent INTEGER REFERENCES nodes(id) ON DELETE RESTRICT,
    name TEXT NOT NULL COLLATE BINARY,
    kind INTEGER NOT NULL CHECK(kind IN (1,2)),
    size INTEGER NOT NULL DEFAULT 0 CHECK(typeof(size)='integer' AND size>=0),
    UNIQUE(parent,name),
    CHECK(kind=1 OR size=0),
    CHECK((id=1 AND parent IS NULL AND name='' AND kind=2) OR
          (id<>1 AND parent IS NOT NULL AND length(name)>0 AND
           name NOT IN ('.','..') AND instr(name,'/')=0))
);
INSERT INTO nodes(id,parent,name,kind,size) VALUES(1,NULL,'',2,0);
CREATE TABLE chunks (
    node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    chunk_index INTEGER NOT NULL CHECK(typeof(chunk_index)='integer' AND chunk_index>=0),
    data BLOB NOT NULL CHECK(typeof(data)='blob' AND length(data) BETWEEN 1 AND 65536),
    PRIMARY KEY(node_id,chunk_index)
) WITHOUT ROWID;
