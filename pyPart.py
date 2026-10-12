import sqlite3
import os
import sys
from flask import Flask, render_template_string, request

app = Flask(__name__)
DB_PATH = "file_system.db"

HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>SQLite File System Explorer</title>
    <style>
        :root {
            --bg: #0f172a;
            --card-bg: #1e293b;
            --text: #f8fafc;
            --text-muted: #94a3b8;
            --accent: #38bdf8;
            --border: #334155;
            --highlight: #0284c7;
        }
        body {
            font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background-color: var(--bg);
            color: var(--text);
            margin: 0;
            padding: 24px;
        }
        .container { max-width: 1200px; margin: 0 auto; }
        header { margin-bottom: 24px; }
        h1 { color: var(--accent); margin: 0 0 8px 0; font-size: 1.8rem; }
        .sub { color: var(--text-muted); font-size: 0.95rem; }
        .stats { display: flex; gap: 16px; margin-bottom: 24px; }
        .card {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 16px 20px;
            flex: 1;
        }
        .card h3 { margin: 0; color: var(--text-muted); font-size: 0.85rem; text-transform: uppercase; letter-spacing: 0.05em; }
        .card .val { font-size: 2rem; font-weight: 700; margin-top: 6px; color: var(--accent); }
        
        .nav-tabs { display: flex; gap: 8px; border-bottom: 1px solid var(--border); margin-bottom: 20px; }
        .tab-btn {
            background: none;
            border: none;
            color: var(--text-muted);
            padding: 10px 18px;
            cursor: pointer;
            font-size: 0.95rem;
            font-weight: 500;
            border-bottom: 2px solid transparent;
            text-decoration: none;
        }
        .tab-btn.active { color: var(--accent); border-bottom-color: var(--accent); font-weight: 600; }
        
        .search-form { display: flex; gap: 10px; margin-bottom: 20px; }
        .search-input {
            flex: 1;
            padding: 10px 14px;
            background: var(--card-bg);
            border: 1px solid var(--border);
            color: var(--text);
            border-radius: 6px;
            font-size: 0.95rem;
        }
        .search-input:focus { outline: none; border-color: var(--accent); }
        .submit-btn {
            background: var(--highlight);
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 6px;
            cursor: pointer;
            font-weight: 600;
        }
        
        table { width: 100%; border-collapse: collapse; background: var(--card-bg); border-radius: 8px; overflow: hidden; border: 1px solid var(--border); }
        th, td { padding: 12px 16px; text-align: left; border-bottom: 1px solid var(--border); font-size: 0.9rem; }
        th { background: #0b1120; color: var(--accent); font-weight: 600; }
        tr:hover { background-color: rgba(56, 189, 248, 0.04); }
        .path-code { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; color: #7dd3fc; word-break: break-all; }
        .badge { background: #334155; padding: 2px 8px; border-radius: 4px; font-size: 0.8rem; color: #cbd5e1; }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>📂 SQLite File System Explorer</h1>
            <div class="sub">Database Source: <code>{{ db_path }}</code></div>
        </header>

        <div class="stats">
            <div class="card">
                <h3>Total Folders</h3>
                <div class="val">{{ "{:,}".format(total_folders) }}</div>
            </div>
            <div class="card">
                <h3>Total Files</h3>
                <div class="val">{{ "{:,}".format(total_files) }}</div>
            </div>
        </div>

        <div class="nav-tabs">
            <a href="/?view=paths" class="tab-btn {{ 'active' if view == 'paths' else '' }}">Reconstructed Paths</a>
            <a href="/?view=files" class="tab-btn {{ 'active' if view == 'files' else '' }}">Files Table</a>
            <a href="/?view=folders" class="tab-btn {{ 'active' if view == 'folders' else '' }}">Folders Table</a>
        </div>

        <form class="search-form" method="GET" action="/">
            <input type="hidden" name="view" value="{{ view }}">
            <input type="text" name="q" class="search-input" placeholder="Search by name or path..." value="{{ search_query }}">
            <button type="submit" class="submit-btn">Filter</button>
        </form>

        <table>
            {% if view == 'paths' %}
                <thead>
                    <tr>
                        <th style="width: 80px;">File ID</th>
                        <th>Full File System Path</th>
                    </tr>
                </thead>
                <tbody>
                    {% for file_id, full_path in rows %}
                    <tr>
                        <td><span class="badge">{{ file_id }}</span></td>
                        <td class="path-code">{{ full_path }}</td>
                    </tr>
                    {% else %}
                    <tr><td colspan="2">No matching paths found.</td></tr>
                    {% endfor %}
                </tbody>
            {% elif view == 'files' %}
                <thead>
                    <tr>
                        <th style="width: 80px;">File ID</th>
                        <th style="width: 100px;">Folder ID</th>
                        <th>File Name</th>
                    </tr>
                </thead>
                <tbody>
                    {% for file_id, folder_id, name in rows %}
                    <tr>
                        <td><span class="badge">{{ file_id }}</span></td>
                        <td>{{ folder_id }}</td>
                        <td>{{ name }}</td>
                    </tr>
                    {% else %}
                    <tr><td colspan="3">No files found.</td></tr>
                    {% endfor %}
                </tbody>
            {% elif view == 'folders' %}
                <thead>
                    <tr>
                        <th style="width: 80px;">Folder ID</th>
                        <th style="width: 100px;">Parent ID</th>
                        <th>Folder Name</th>
                    </tr>
                </thead>
                <tbody>
                    {% for folder_id, parent_id, name in rows %}
                    <tr>
                        <td><span class="badge">{{ folder_id }}</span></td>
                        <td>{{ parent_id }}</td>
                        <td>{{ name }}</td>
                    </tr>
                    {% else %}
                    <tr><td colspan="3">No folders found.</td></tr>
                    {% endfor %}
                </tbody>
            {% endif %}
        </table>
    </div>
</body>
</html>
"""

def decode_text(b):
    try:
        return b.decode('utf-8')
    except UnicodeDecodeError:
        # Fall back to Windows CP1252, or replace invalid characters gracefully
        return b.decode('cp1252', errors='replace')

def get_db_connection():
    conn = sqlite3.connect(DB_PATH)
    # Override default UTF-8 text factory to prevent crashes
    conn.text_factory = decode_text
    conn.row_factory = sqlite3.Row
    return conn

@app.route("/")
def index():
    if not os.path.exists(DB_PATH):
        return f"<h2>Error: '{DB_PATH}' not found. Run your C crawler program first!</h2>"

    view = request.args.get("view", "paths")
    query = request.args.get("q", "").strip()

    conn = get_db_connection()
    cursor = conn.cursor()

    # Get Counts
    total_folders = cursor.execute("SELECT COUNT(*) FROM folders").fetchone()[0]
    total_files = cursor.execute("SELECT COUNT(*) FROM files").fetchone()[0]

    rows = []
    
    if view == "files":
        if query:
            rows = cursor.execute("SELECT file_id, folder_id, name FROM files WHERE name LIKE ? LIMIT 200", (f"%{query}%",)).fetchall()
        else:
            rows = cursor.execute("SELECT file_id, folder_id, name FROM files LIMIT 200").fetchall()

    elif view == "folders":
        if query:
            rows = cursor.execute("SELECT folder_id, parent_id, name FROM folders WHERE name LIKE ? LIMIT 200", (f"%{query}%",)).fetchall()
        else:
            rows = cursor.execute("SELECT folder_id, parent_id, name FROM folders LIMIT 200").fetchall()

    else:  # view == 'paths'
        cte_sql = """
        WITH RECURSIVE FolderPath(folder_id, full_path) AS (
            SELECT folder_id, name FROM folders WHERE parent_id = 0
            UNION ALL
            SELECT f.folder_id, fp.full_path || '/' || f.name
            FROM folders f JOIN FolderPath fp ON f.parent_id = fp.folder_id
        )
        SELECT fi.file_id, fp.full_path || '/' || fi.name AS full_file_path
        FROM files fi JOIN FolderPath fp ON fi.folder_id = fp.folder_id
        """
        if query:
            sql = cte_sql + " WHERE full_file_path LIKE ? LIMIT 200;"
            rows = cursor.execute(sql, (f"%{query}%",)).fetchall()
        else:
            sql = cte_sql + " LIMIT 200;"
            rows = cursor.execute(sql).fetchall()

    conn.close()

    return render_template_string(
        HTML_TEMPLATE,
        db_path=DB_PATH,
        total_folders=total_folders,
        total_files=total_files,
        view=view,
        search_query=query,
        rows=rows
    )

if __name__ == "__main__":
    if len(sys.argv) > 1:
        DB_PATH = sys.argv[1]

    print("=" * 60)
    print("  🚀 Starting Localhost SQLite Web Explorer")
    print("  Open your browser to: http://127.0.0.1:5000")
    print("=" * 60)
    app.run(host="127.0.0.1", port=5000, debug=True)