CREATE TABLE IF NOT EXISTS folders (
    folder_id INTEGER PRIMARY KEY,
    parent_id INTEGER,
    name TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS files (
    file_id INTEGER PRIMARY KEY,
    folder_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    tag_mask INTEGER DEFAULT 0,
    FOREIGN KEY(folder_id) REFERENCES folders(folder_id)
);

CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);
CREATE INDEX IF NOT EXISTS idx_files_folder ON files(folder_id);