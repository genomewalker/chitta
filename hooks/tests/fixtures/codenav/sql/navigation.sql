CREATE TABLE reads (id INTEGER PRIMARY KEY, sample_id INTEGER);
CREATE TABLE counts (read_id INTEGER REFERENCES reads(id), total INTEGER);
CREATE VIEW read_totals AS SELECT reads.id, counts.total FROM reads JOIN counts ON reads.id = counts.read_id;
CREATE FUNCTION read_count() RETURNS INTEGER LANGUAGE SQL AS $$ SELECT count(*) FROM reads; $$;
