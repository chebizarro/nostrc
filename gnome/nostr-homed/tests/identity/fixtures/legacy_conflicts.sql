PRAGMA user_version=0;
CREATE TABLE users(uid INTEGER PRIMARY KEY,npub TEXT UNIQUE,username TEXT UNIQUE,
  gid INTEGER,home TEXT,updated_at INTEGER);
CREATE TABLE groups(gid INTEGER PRIMARY KEY,name TEXT UNIQUE);
INSERT INTO users VALUES
 (100101,'npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq','alice',100101,'/home/shared',1),
 (100102,'not-a-nip19-key','bob',100102,'/home/shared',1),
 (100103,'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','n_valid',100104,'relative/home',1);
INSERT INTO groups VALUES (100101,'wrong-owner'),(100102,'bob');
CREATE TABLE migration_evidence(kind TEXT NOT NULL,subject TEXT NOT NULL,
  detail TEXT NOT NULL);
INSERT INTO migration_evidence VALUES
 ('overwritten_uid','100103','surviving legacy row cannot prove rightful owner'),
 ('mounted_home','alice','/home/shared has roaming mount evidence');
