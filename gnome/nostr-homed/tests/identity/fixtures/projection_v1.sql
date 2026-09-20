PRAGMA application_id=1313361969;
PRAGMA user_version=1;
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE passwd(name TEXT PRIMARY KEY,uid INTEGER UNIQUE NOT NULL,
  gid INTEGER NOT NULL,gecos TEXT NOT NULL,home TEXT NOT NULL,
  shell TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE grp(name TEXT PRIMARY KEY,gid INTEGER UNIQUE NOT NULL) WITHOUT ROWID;
INSERT INTO meta VALUES
 ('authority_id','11111111-1111-4111-8111-111111111111'),
 ('projection_generation','7'),('source_authority_generation','12'),
 ('format_minor','0'),('built_at','1789900000');
INSERT INTO passwd VALUES
 ('n_active',200000,200000,'Nostr User','/home/n_active','/bin/bash'),
 ('n_disabled',200001,200001,'Nostr User','/home/n_disabled','/bin/bash');
INSERT INTO grp VALUES ('n_active',200000),('n_disabled',200001);
