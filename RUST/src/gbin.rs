use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use gbin::*;
use std::collections::BTreeMap;
use std::io::{Read};
use std::path::{Path, PathBuf};

use crossterm::{
    event::{self, Event, KeyCode, KeyEvent, KeyModifiers,
        DisableMouseCapture, EnableMouseCapture, MouseEventKind},
    execute,
    style::Stylize,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Paragraph, Wrap},
    Terminal,
};
use std::time::Duration;

#[derive(Parser, Debug)]
#[command(name = "gbin", version, about = "GBF/GREDBIN inspector (header/tree/show)")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Print header summary (or raw JSON)
    Header {
        file: PathBuf,
        /// Print raw header JSON as stored in file
        #[arg(long)]
        raw: bool,
        /// Pretty-print header JSON
        #[arg(long)]
        pretty: bool,
        /// Validate by forcing a full-file read with CRC checks (slow for large files)
        #[arg(long)]
        validate: bool,
    },

    /// Print variable tree (from header only; fast)
    Tree {
        file: PathBuf,
        /// Only show subtree under this prefix (dot-separated)
        #[arg(long)]
        prefix: Option<String>,
        /// Max depth to print (default: unlimited)
        #[arg(long, default_value_t = usize::MAX)]
        max_depth: usize,
        /// Show additional leaf details (compression/offset/sizes)
        #[arg(long)]
        details: bool,
        /// Validate by forcing a full-file read with CRC checks (slow for large files)
        #[arg(long)]
        validate: bool,
    },

    /// Read and print the content of a variable (bounded preview)
    Show {
        file: PathBuf,
        /// Variable path (dot-separated). If omitted, starts at the root.
        var: Option<String>,
        /// Linear preview: maximum elements to print
        #[arg(long, default_value_t = 20)]
        max_elems: usize,
        /// For 2D numeric arrays: print a top-left matrix preview
        #[arg(long, default_value_t = 6)]
        rows: usize,
        /// For 2D numeric arrays: print a top-left matrix preview
        #[arg(long, default_value_t = 6)]
        cols: usize,
        /// Compute basic stats for numeric arrays over the full array (can be slow)
        #[arg(long)]
        stats: bool,
        /// Validate CRCs while reading
        #[arg(long)]
        validate: bool,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    match cli.cmd {
        Cmd::Header {
            file,
            raw,
            pretty,
            validate,
        } => cmd_header(&file, raw, pretty, validate),

        Cmd::Tree {
            file,
            prefix,
            max_depth,
            details,
            validate,
        } => cmd_tree(&file, prefix.as_deref(), max_depth, details, validate),

        Cmd::Show {
            file,
            var,
            max_elems,
            rows,
            cols,
            stats,
            validate,
        } => {
            let var = var.as_deref().unwrap_or("");
            cmd_show(&file, var, max_elems, rows, cols, stats, validate)
        }
    }
}

//
// ===== Header parsing (fast; does not touch payload) =====
// File layout: [8 magic][u32 header_len LE][header_json bytes][payload bytes...]
//


//
// ===== Commands =====
//

fn cmd_header(path: &std::path::Path, raw: bool, pretty: bool, validate: bool) -> Result<()> {
    if validate {
        // Full file validation (can be expensive, but definitive)
        let _ = read_file(path, ReadOptions { validate: true })
            .with_context(|| "validate failed")?;
    }

    // Read header using library API
	let (hdr, header_len, raw_json) = gbin::read_header_only(path, gbin::ReadOptions { validate: true })?;

    // Helper: read magic from file
    fn file_magic(path: &std::path::Path) -> Result<String> {
        let mut f = std::fs::File::open(path)?;
        let mut magic = [0u8; 8];
        f.read_exact(&mut magic)?;
        Ok(String::from_utf8_lossy(&magic).trim_end_matches('\0').to_string())
    }
    let magic_s = file_magic(path)?;

    if magic_s != "GREDBIN" {
        eprintln!(
            "{} {} '{}' {} {}",
            "warning".yellow().bold(),
            ": file magic is".dim(),
            magic_s.as_str().red().bold(),
            ", expected".dim(),
            "GREDBIN".green().bold(),
        );
    }

    println!(
        "{} {}",
        "file".cyan().bold(),
        path.display().to_string().white().bold()
    );
    println!(
        "{} {}",
        "magic".cyan().bold(),
        magic_s.as_str().green().bold()
    );
    println!(
        "{} {} {}",
        "header".cyan().bold(),
        "len".cyan().bold(),
        format!("{header_len} bytes").white().bold()
    );

    if raw {
        if pretty {
            let v: serde_json::Value = serde_json::from_str(&raw_json)?;
            println!("{}", serde_json::to_string_pretty(&v)?);
        } else {
            println!("{}", raw_json);
        }
        return Ok(());
    }

    // Summary view: print known fields, fallback to pretty JSON if fields are missing
    // Try to print fields from the Header struct; fallback to pretty JSON if needed.
    let summary = serde_json::to_string_pretty(&hdr)?;
    println!("{}", summary);

    Ok(())
}

#[derive(Default)]
struct TreeNode {
    children: BTreeMap<String, TreeNode>,
    leaf_idx: Option<usize>,
}

fn tree_insert(root: &mut TreeNode, path: &str, idx: usize) {
    let mut cur = root;
    for part in path.split('.') {
        cur = cur.children.entry(part.to_string()).or_default();
    }
    cur.leaf_idx = Some(idx);
}

fn tree_find<'a>(root: &'a TreeNode, prefix: &str) -> Option<&'a TreeNode> {
    let mut cur = root;
    for part in prefix.split('.') {
        cur = cur.children.get(part)?;
    }
    Some(cur)
}

fn fmt_shape(shape: &[u64]) -> String {
    if shape.is_empty() {
        return "[?]".to_string();
    }
    let parts: Vec<String> = shape.iter().map(|d| d.to_string()).collect();
    format!("[{}]", parts.join(" x "))
}

fn fmt_shape_usize(shape: &[usize]) -> String {
    if shape.is_empty() {
        return "[?]".to_string();
    }
    let parts: Vec<String> = shape.iter().map(|d| d.to_string()).collect();
    format!("[{}]", parts.join(" x "))
}

// Helper: Get the shape hint for a given path from the fields.
fn shape_hint_for_path(fields: &[gbin::FieldMeta], path: &str) -> Option<String> {
    fields
        .iter()
        .find(|f| f.name == path)
        .map(|f| fmt_shape(&f.shape))
}

// Helper: Convert CharArray to a readable string
fn chararray_to_display(c: &CharArray) -> String {
    // MATLAB char is stored as UTF-16 code units.
    // The library stores the decoded code units in `data`.
    let units: Vec<u16> = c.data.iter().map(|&x| x as u16).collect();
    // Keep embedded NULs if any (rare), but trim trailing NULs which can come from padding.
    let mut s = String::from_utf16_lossy(&units);
    while s.ends_with('\u{0}') {
        s.pop();
    }
    s
}

fn print_tree(
    node: &TreeNode,
    fields: &[gbin::FieldMeta],
    indent: usize,
    max_depth: usize,
    details: bool,
) {
    if indent / 2 >= max_depth {
        return;
    }

    for (name, child) in &node.children {
        let pad = " ".repeat(indent);

        if !child.children.is_empty() && child.leaf_idx.is_none() {
            println!(
                "{}{}{}",
                pad,
                name.as_str().cyan().bold(),
                "/".dim()
            );
            print_tree(child, fields, indent + 2, max_depth, details);
            continue;
        }

        // Leaf (or mixed)
        if let Some(i) = child.leaf_idx {
            let f = &fields[i];
            let kind = f.kind.as_str();
            let class = f.class_name.as_str();
            let shape = fmt_shape(&f.shape);

            if details {
                let comp = f.compression.as_str();
                let off = f.offset;
                let csize = f.csize;
                let usize_ = f.usize;
                let complex = f.complex;
                let enc = f.encoding.as_str();
                let crc = f.crc32;

                // Keep the output compact and stable for grepping.
                if enc.is_empty() {
                    println!(
                        "{}{} {} {} {} {} {} {} {} {} {}",
                        pad,
                        format!("{:<24}", name).white().bold(),
                        format!("{:<12}", shape).dim(),
                        format!("{:<10}", class).yellow().bold(),
                        format!("kind={:<10}", kind).dim(),
                        format!("complex={}", complex).dim(),
                        format!("comp={:<8}", comp).dim(),
                        format!("off={}", off).dim(),
                        format!("csize={}", csize).dim(),
                        format!("usize={}", usize_).dim(),
                        format!("crc32={:08X}", crc).green().bold(),
                    );
                } else {
                    println!(
                        "{}{} {} {} {} {} {} {} {} {} {} {}",
                        pad,
                        format!("{:<24}", name).white().bold(),
                        format!("{:<12}", shape).dim(),
                        format!("{:<10}", class).yellow().bold(),
                        format!("kind={:<10}", kind).dim(),
                        format!("complex={}", complex).dim(),
                        format!("enc={:<24}", enc).dim(),
                        format!("comp={:<8}", comp).dim(),
                        format!("off={}", off).dim(),
                        format!("csize={}", csize).dim(),
                        format!("usize={}", usize_).dim(),
                        format!("crc32={:08X}", crc).green().bold(),
                    );
                }
            } else {
                println!(
                    "{}{} {:<12} {}",
                    pad,
                    format!("{:<24}", name).white().bold(),
                    shape.dim(),
                    class.yellow().bold(),
                );
            }
        }

        // If it also has children, show as directory too.
        if !child.children.is_empty() {
            println!(
                "{}{}{}",
                pad,
                name.as_str().cyan().bold(),
                "/".dim()
            );
            print_tree(child, fields, indent + 2, max_depth, details);
        }
    }
}

fn cmd_tree(path: &std::path::Path, prefix: Option<&str>, max_depth: usize, details: bool, validate: bool) -> Result<()> {
    if validate {
        let _ = read_file(path, ReadOptions { validate: true })
            .with_context(|| "validate failed")?;
    }

    let (hdr, _header_len, _raw_json) = gbin::read_header_only(path, gbin::ReadOptions { validate: true })?;

    let mut root = TreeNode::default();
    for (i, f) in hdr.fields.iter().enumerate() {
        tree_insert(&mut root, &f.name, i);
    }

    println!(
        "{} {}",
        "gbf".magenta().bold(),
        format!("variable tree: {}", path.display()).white().bold()
    );

    if let Some(pfx) = prefix {
        if let Some(node) = tree_find(&root, pfx) {
            println!("(prefix: {})", pfx);
            print_tree(node, &hdr.fields, 0, max_depth, details);
            return Ok(());
        } else {
            bail!("prefix '{}' not found in header fields", pfx);
        }
    }

    print_tree(&root, &hdr.fields, 0, max_depth, details);
    Ok(())
}

fn cmd_show(
    path: &Path,
    var: &str,
    max_elems: usize,
    rows: usize,
    cols: usize,
    stats: bool,
    validate: bool,
) -> Result<()> {
    // We'll implement SHOW as an interactive tree inspector rooted at `var`.
    // It uses only the header to build the tree, and reads a variable on Enter.
    let ropts = ReadOptions { validate };

    // Read header (fast) so we can build the subtree.
    let (hdr, _header_len, _raw_json) =
        gbin::read_header_only(path, gbin::ReadOptions { validate: true })?;

    // Build tree for all fields, then select subtree rooted at `var` (prefix).
    let mut root = TreeNode::default();
    for (i, f) in hdr.fields.iter().enumerate() {
        tree_insert(&mut root, &f.name, i);
    }

    // Resolve the prefix node. If `var` is exactly a leaf, show only that leaf.
    let subtree = if var.is_empty() {
        &root
    } else {
        tree_find(&root, var).ok_or_else(|| anyhow::anyhow!("var/prefix '{}' not found", var.red().bold()))?
    };

    // Flatten visible nodes based on expansion state.
    let mut state = ShowState::new(path.to_path_buf(), var.to_string(), max_elems, rows, cols, stats, ropts, hdr.fields.clone());
    state.load_tree_from(subtree, var);

    run_show_tui(&mut state)
}

//
// ===== Value preview =====
//

fn shape_numel(shape: &[usize]) -> usize {
    shape.iter().copied().product::<usize>()
}

fn numeric_class_key(class: &NumericClass) -> String {
    // Avoid hardcoding enum variants; this makes the CLI resilient.
    format!("{:?}", class).to_lowercase()
}

fn bytes_per_elem(class_key: &str) -> Option<usize> {
    match class_key {
        "double" => Some(8),
        "single" => Some(4),
        "int8" => Some(1),
        "uint8" => Some(1),
        "int16" => Some(2),
        "uint16" => Some(2),
        "int32" => Some(4),
        "uint32" => Some(4),
        "int64" => Some(8),
        "uint64" => Some(8),
        _ => None,
    }
}

fn decode_scalar_to_string(class_key: &str, bytes: &[u8]) -> String {
    // bytes slice is exactly element-size, little-endian.
    match class_key {
        "double" => {
            let mut a = [0u8; 8];
            a.copy_from_slice(bytes);
            let v = f64::from_le_bytes(a);
            if v == 0.0 {
                "0".to_string()
            } else {
                let av = v.abs();
                if av < 1e-3 || av >= 1e6 {
                    format!("{:.6e}", v)
                } else {
                    let s = format!("{:.6}", v);
                    s.trim_end_matches('0')
                        .trim_end_matches('.')
                        .to_string()
                }
            }
        }
        "single" => {
            let mut a = [0u8; 4];
            a.copy_from_slice(bytes);
            let v = f32::from_le_bytes(a) as f64;
            if v == 0.0 {
                "0".to_string()
            } else {
                let av = v.abs();
                if av < 1e-3 || av >= 1e6 {
                    format!("{:.6e}", v)
                } else {
                    let s = format!("{:.6}", v);
                    s.trim_end_matches('0')
                        .trim_end_matches('.')
                        .to_string()
                }
            }
        }
        "int8" => i8::from_le_bytes([bytes[0]]).to_string(),
        "uint8" => u8::from_le_bytes([bytes[0]]).to_string(),
        "int16" => {
            let mut a = [0u8; 2];
            a.copy_from_slice(bytes);
            i16::from_le_bytes(a).to_string()
        }
        "uint16" => {
            let mut a = [0u8; 2];
            a.copy_from_slice(bytes);
            u16::from_le_bytes(a).to_string()
        }
        "int32" => {
            let mut a = [0u8; 4];
            a.copy_from_slice(bytes);
            i32::from_le_bytes(a).to_string()
        }
        "uint32" => {
            let mut a = [0u8; 4];
            a.copy_from_slice(bytes);
            u32::from_le_bytes(a).to_string()
        }
        "int64" => {
            let mut a = [0u8; 8];
            a.copy_from_slice(bytes);
            i64::from_le_bytes(a).to_string()
        }
        "uint64" => {
            let mut a = [0u8; 8];
            a.copy_from_slice(bytes);
            u64::from_le_bytes(a).to_string()
        }
        _ => {
            // Fallback: hex
            let s: Vec<String> = bytes.iter().map(|b| format!("{:02X}", b)).collect();
            format!("0x{}", s.join(""))
        }
    }
}



// ===== Interactive SHOW TUI =====

#[derive(Clone, Debug)]
struct UiNode {
    label: String,
    full_path: String,
    is_leaf: bool,
    children: Vec<UiNode>,
}

impl UiNode {
    fn new_branch(label: String, full_path: String, children: Vec<UiNode>) -> Self {
        Self {
            label,
            full_path,
            is_leaf: false,
            children,
        }
    }

    fn new_leaf(label: String, full_path: String) -> Self {
        Self {
            label,
            full_path,
            is_leaf: true,
            children: vec![],
        }
    }
}

#[derive(Clone, Debug)]
struct FlatRow {
    depth: usize,
    node_path: String,
    label: String,
    is_leaf: bool,
    expanded: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Focus {
    Tree,
    Preview,
}

#[derive(Debug)]
struct ShowState {
    file: PathBuf,
    root_prefix: String,

    max_elems: usize,
    rows: usize,
    cols: usize,
    stats: bool,
    ropts: ReadOptions,

    // Metadata (from header)
    fields: Vec<gbin::FieldMeta>,

    // Tree model
    ui_root: UiNode,
    expanded: std::collections::BTreeMap<String, bool>,
    flat: Vec<FlatRow>,
    selected: usize,

    // Scroll offsets (number of lines from top)
    tree_scroll: u16,
    preview_scroll: u16,

    // Which panel receives scroll/key focus.
    focus: Focus,

    // Preview panel state
    preview_title: String,
    preview_lines: Vec<String>,
    last_error: Option<String>,
}

impl ShowState {
    fn new(
        file: PathBuf,
        root_prefix: String,
        max_elems: usize,
        rows: usize,
        cols: usize,
        stats: bool,
        ropts: ReadOptions,
        fields: Vec<gbin::FieldMeta>,
    ) -> Self {
        Self {
            file,
            root_prefix,
            max_elems,
            rows,
            cols,
            stats,
            ropts,
            fields,
            ui_root: UiNode::new_branch("<root>".to_string(), "".to_string(), vec![]),
            expanded: BTreeMap::new(),
            flat: vec![],
            selected: 0,
            tree_scroll: 0,
            preview_scroll: 0,
            focus: Focus::Tree,
            preview_title: "Preview".to_string(),
            preview_lines: vec![
                "↑/↓ move  → expand  ← collapse  Enter preview  Tab focus  PgUp/PgDn scroll  mouse wheel  q quit".to_string(),
                "".to_string(),
                "Select a node and press Enter.".to_string(),
            ],
            last_error: None,
        }
    }

    fn load_tree_from(&mut self, subtree: &TreeNode, prefix: &str) {
        // Convert TreeNode -> UiNode recursively.
        self.ui_root = build_ui_node(subtree, prefix.to_string());
        // Default expansion: expand root prefix
        if !prefix.is_empty() {
            self.expanded.insert(prefix.to_string(), true);
        }
        self.recompute_flat();
        self.selected = 0;
    }

    fn recompute_flat(&mut self) {
        self.flat.clear();
        let base = self.ui_root.full_path.clone();
        // Expand root always
        self.expanded.insert(base.clone(), true);
        flatten_visible(&self.ui_root, 0, &mut self.flat, &self.expanded);

        if self.selected >= self.flat.len() && !self.flat.is_empty() {
            self.selected = self.flat.len() - 1;
        }
    }

    fn selected_row(&self) -> Option<&FlatRow> {
        self.flat.get(self.selected)
    }

    fn toggle_expand_selected(&mut self, expand: bool) {
        let Some(row) = self.selected_row() else { return; };
        if row.is_leaf {
            return;
        }
        self.expanded.insert(row.node_path.clone(), expand);
        self.recompute_flat();
    }

    fn move_sel(&mut self, delta: i32) {
        if self.flat.is_empty() {
            self.selected = 0;
            self.tree_scroll = 0;
            return;
        }
        let cur = self.selected as i32;
        let next = (cur + delta).clamp(0, (self.flat.len() - 1) as i32);
        self.selected = next as usize;
        // scroll adjusted during draw based on viewport height, but reset if list shrinks
        if self.selected == 0 {
            self.tree_scroll = 0;
        }
        // Ensure selected stays visible (actual viewport height applied in draw via clamp).
        // Here we just keep scroll from drifting too far above selection.
        let sel = self.selected as u16;
        if sel < self.tree_scroll {
            self.tree_scroll = sel;
        }
    }

    fn preview_selected(&mut self) {
        let (is_leaf, node_path) = {
            let Some(row) = self.selected_row() else { return; };
            (row.is_leaf, row.node_path.clone())
        };

        // If it’s a leaf, show field meta + read and preview.
        // If it’s a branch, show children summary.
        self.last_error = None;

        if is_leaf {
            self.preview_title = format!("{}  (leaf)", node_path);

            let meta = self
                .fields
                .iter()
                .find(|f| f.name == node_path);

            let mut lines = vec![];

            if let Some(f) = meta {
                // One property per line: stable, readable, and avoids whitespace tokenization issues.
                lines.push(format!("kind = {}", f.kind));
                lines.push(format!("class = {}", f.class_name));
                lines.push(format!("shape = {}", fmt_shape(&f.shape)));
                lines.push(format!("complex = {}", f.complex));
                lines.push(format!("comp = {}", f.compression));
                lines.push(format!("off = {}", f.offset));
                lines.push(format!("csize = {}", f.csize));
                lines.push(format!("usize = {}", f.usize));
                lines.push(format!("crc32 = {:08X}", f.crc32));
                if !f.encoding.is_empty() {
                    lines.push(format!("encoding = {}", f.encoding));
                }
                lines.push("".to_string());
            }

            self.preview_scroll = 0;

            match read_var(&self.file, &node_path, self.ropts.clone()) {
                Ok(v) => {
                    // Render preview into lines
                    let mut rendered = render_value_preview(&v, self.max_elems, self.rows, self.cols, self.stats);

                    // We already printed the header metadata above (kind/class/shape/etc.).
                    // Avoid repeating it in the value preview when possible.
                    if let Some(first) = rendered.first() {
                        if first.starts_with("numeric:")
                            || first.starts_with("logical:")
                            || first.starts_with("string:")
                            || first.starts_with("categorical:")
                            || first.starts_with("struct:")
                        {
                            rendered.remove(0);
                        }
                    }

                    // If we removed the first line and the next line is empty, drop that too.
                    if rendered.first().map(|s| s.trim().is_empty()).unwrap_or(false) {
                        rendered.remove(0);
                    }

                    lines.extend(rendered);
                }
                Err(e) => {
                    self.last_error = Some(format!("{e:#}"));
                    lines.push(format!("ERROR: {e:#}"));
                }
            }

            self.preview_lines = lines;
        } else {
            self.preview_title = format!("{}  (branch)", node_path);
            self.preview_scroll = 0;
            self.preview_lines = vec![
                "Press → to expand, ← to collapse.".to_string(),
                "Press Enter on leaves to load data preview.".to_string(),
            ];
        }
    }
}

fn build_ui_node(tree: &TreeNode, prefix: String) -> UiNode {
    // `prefix` is the full dotted path to this node.
    // Children are sorted by key.
    let mut children: Vec<UiNode> = vec![];

    for (name, child) in tree.children.iter() {
        let full = if prefix.is_empty() {
            name.to_string()
        } else {
            format!("{prefix}.{name}")
        };

        // If child is a pure leaf:
        if child.children.is_empty() {
            if child.leaf_idx.is_some() {
                children.push(UiNode::new_leaf(name.to_string(), full));
            } else {
                // Weird: no children and no leaf idx => treat as leaf-ish
                children.push(UiNode::new_leaf(name.to_string(), full));
            }
            continue;
        }

        // Mixed node: can have both leaf_idx and children.
        // We represent it as a branch. (Enter will not load it unless it’s also a leaf path)
        let branch = build_ui_node(child, full.clone());

        if child.leaf_idx.is_some() {
            // Represent the leaf as an artificial child called "<value>" so it’s reachable.
            let leaf_full = full.clone();
            let leaf_label = "<value>".to_string();
            let mut branch_children = branch.children;
            branch_children.insert(0, UiNode::new_leaf(leaf_label, leaf_full));
            children.push(UiNode::new_branch(name.to_string(), full, branch_children));
        } else {
            children.push(UiNode::new_branch(name.to_string(), full, branch.children));
        }
    }

    // Sort: branches first, then leaves; stable by label
    children.sort_by(|a, b| {
        match (a.is_leaf, b.is_leaf) {
            (false, true) => std::cmp::Ordering::Less,
            (true, false) => std::cmp::Ordering::Greater,
            _ => a.label.cmp(&b.label),
        }
    });

    // Root node label is the last component of prefix.
    let label = if prefix.is_empty() {
        "<root>".to_string()
    } else {
        prefix.split('.').last().unwrap_or(&prefix).to_string()
    };

    UiNode::new_branch(label, prefix, children)
}

fn clamp_scroll(scroll: u16, content_len: usize, viewport_h: u16) -> u16 {
    if viewport_h == 0 {
        return 0;
    }
    let content_len = content_len as u16;
    if content_len <= viewport_h {
        return 0;
    }
    let max_scroll = content_len.saturating_sub(viewport_h);
    scroll.min(max_scroll)
}

fn ensure_visible(scroll: u16, sel: u16, viewport_h: u16) -> u16 {
    if viewport_h == 0 {
        return scroll;
    }
    let top = scroll;
    let bottom = scroll.saturating_add(viewport_h.saturating_sub(1));
    if sel < top {
        sel
    } else if sel > bottom {
        sel.saturating_sub(viewport_h.saturating_sub(1))
    } else {
        scroll
    }
}

fn flatten_visible(node: &UiNode, depth: usize, out: &mut Vec<FlatRow>, expanded: &BTreeMap<String, bool>) {
    // Skip the artificial root label from printing if it’s empty prefix.
    if !node.full_path.is_empty() {
        let is_expanded = expanded.get(&node.full_path).copied().unwrap_or(false);
        out.push(FlatRow {
            depth,
            node_path: node.full_path.clone(),
            label: node.label.clone(),
            is_leaf: node.is_leaf,
            expanded: is_expanded,
        });
    }

    if node.is_leaf {
        return;
    }

    let is_expanded = if node.full_path.is_empty() {
        true
    } else {
        expanded.get(&node.full_path).copied().unwrap_or(false)
    };

    if !is_expanded {
        return;
    }

    for ch in &node.children {
        flatten_visible(ch, depth + 1, out, expanded);
    }
}

fn run_show_tui(state: &mut ShowState) -> Result<()> {
    enable_raw_mode()?;
    let mut stdout = std::io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    execute!(stdout, EnableMouseCapture)?;

    let backend = CrosstermBackend::new(stdout);
    let mut term = Terminal::new(backend)?;

    // Start with a preview of currently selected row (if any).
    if !state.flat.is_empty() {
        state.preview_selected();
    }

    let res = (|| -> Result<()> {
        loop {
            term.draw(|f| draw_show_ui(f, state))?;

            // Poll for input
            if event::poll(Duration::from_millis(120))? {
                match event::read()? {
                    Event::Key(KeyEvent { code, modifiers, .. }) => {
                        // Ctrl+C exits too
                        if code == KeyCode::Char('c') && modifiers.contains(KeyModifiers::CONTROL) {
                            break;
                        }

                        match code {
                            KeyCode::Char('q') | KeyCode::Esc => break,
                            KeyCode::Up => state.move_sel(-1),
                            KeyCode::Down => state.move_sel(1),
                            KeyCode::Right => state.toggle_expand_selected(true),
                            KeyCode::Left => state.toggle_expand_selected(false),
                            KeyCode::Enter => state.preview_selected(),
                            KeyCode::PageUp => {
                                match state.focus {
                                    Focus::Tree => state.tree_scroll = state.tree_scroll.saturating_sub(5),
                                    Focus::Preview => state.preview_scroll = state.preview_scroll.saturating_sub(5),
                                }
                            }
                            KeyCode::PageDown => {
                                match state.focus {
                                    Focus::Tree => state.tree_scroll = state.tree_scroll.saturating_add(5),
                                    Focus::Preview => state.preview_scroll = state.preview_scroll.saturating_add(5),
                                }
                            }
                            KeyCode::Tab => {
                                state.focus = match state.focus {
                                    Focus::Tree => Focus::Preview,
                                    Focus::Preview => Focus::Tree,
                                };
                            }
                            KeyCode::BackTab => {
                                state.focus = match state.focus {
                                    Focus::Tree => Focus::Preview,
                                    Focus::Preview => Focus::Tree,
                                };
                            }
                            KeyCode::Char('w') => {
                                if state.focus == Focus::Preview {
                                    state.preview_scroll = state.preview_scroll.saturating_sub(1);
                                }
                            }
                            KeyCode::Char('s') => {
                                if state.focus == Focus::Preview {
                                    state.preview_scroll = state.preview_scroll.saturating_add(1);
                                }
                            }
                            KeyCode::Home => {
                                state.selected = 0;
                                state.tree_scroll = 0;
                            }
                            KeyCode::End => {
                                if !state.flat.is_empty() {
                                    state.selected = state.flat.len() - 1;
                                }
                            }
                            _ => {}
                        }
                    }
                    Event::Mouse(me) => {
                        match me.kind {
                            MouseEventKind::ScrollUp => {
                                match state.focus {
                                    Focus::Tree => state.tree_scroll = state.tree_scroll.saturating_sub(1),
                                    Focus::Preview => state.preview_scroll = state.preview_scroll.saturating_sub(1),
                                }
                            }
                            MouseEventKind::ScrollDown => {
                                match state.focus {
                                    Focus::Tree => state.tree_scroll = state.tree_scroll.saturating_add(1),
                                    Focus::Preview => state.preview_scroll = state.preview_scroll.saturating_add(1),
                                }
                            }
                            _ => {}
                        }
                    }
                    Event::Resize(_, _) => {
                        // handled by redraw
                    }
                    _ => {}
                }
            }
        }
        Ok(())
    })();

    disable_raw_mode()?;
    execute!(term.backend_mut(), DisableMouseCapture)?;
    execute!(term.backend_mut(), LeaveAlternateScreen)?;
    term.show_cursor()?;

    res
}

fn draw_show_ui(f: &mut ratatui::Frame<'_>, state: &ShowState) {
    let size = f.area();

    let chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(45), Constraint::Percentage(55)])
        .split(size);

    draw_tree_panel(f, chunks[0], state);
    draw_preview_panel(f, chunks[1], state);
}

fn draw_tree_panel(f: &mut ratatui::Frame<'_>, area: Rect, state: &ShowState) {
    let title = format!(
        "gbin show  file={}  root={}",
        state.file.display(),
        if state.root_prefix.is_empty() { "<root>" } else { &state.root_prefix }
    );

    let block = Block::default().title(title).borders(Borders::ALL);

    let mut lines: Vec<Line> = vec![];

    if state.flat.is_empty() {
        lines.push(Line::from("No nodes."));
    } else {
        for (i, row) in state.flat.iter().enumerate() {
            let selected = i == state.selected;

            let indent = "  ".repeat(row.depth);
            let glyph = if row.is_leaf {
                "•"
            } else if row.expanded {
                "▾"
            } else {
                "▸"
            };

            let name_style = if selected {
                Style::default()
                    .fg(Color::Black)
                    .bg(Color::Cyan)
                    .add_modifier(Modifier::BOLD)
            } else if row.is_leaf {
                Style::default().fg(Color::White)
            } else {
                Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD)
            };

            let glyph_style = if row.is_leaf {
                Style::default().fg(Color::Magenta)
            } else {
                Style::default().fg(Color::Blue).add_modifier(Modifier::BOLD)
            };

            let mut spans = vec![
                Span::raw(indent),
                Span::styled(format!("{glyph} "), glyph_style),
                Span::styled(row.label.clone(), name_style),
            ];

            // Show dimensions for leaves instead of the generic "leaf" marker.
            if row.is_leaf {
                if let Some(shape) = shape_hint_for_path(&state.fields, &row.node_path) {
                    spans.push(Span::raw(" "));
                    spans.push(Span::styled(shape, Style::default().fg(Color::DarkGray)));
                }
            } else {
                spans.push(Span::raw(" "));
                spans.push(Span::styled("[node]", Style::default().fg(Color::DarkGray)));
            }

            lines.push(Line::from(spans));
        }
    }

    let inner_h = area.height.saturating_sub(2);
    let scroll = clamp_scroll(state.tree_scroll, lines.len(), inner_h);
    let scroll = if !state.flat.is_empty() {
        ensure_visible(scroll, state.selected as u16, inner_h)
    } else {
        scroll
    };

    let paragraph = Paragraph::new(lines)
        .block(block)
        .wrap(Wrap { trim: false })
        .scroll((scroll, 0));
    f.render_widget(paragraph, area);
}

fn draw_preview_panel(f: &mut ratatui::Frame<'_>, area: Rect, state: &ShowState) {
    let mut title = state.preview_title.clone();
    if let Some(err) = &state.last_error {
        title = format!("{title}  (error)");
        // err is printed in body anyway
        let _ = err;
    }

    let block = Block::default()
        .title(title)
        .borders(Borders::ALL);

    let mut lines: Vec<Line> = vec![];

    for s in &state.preview_lines {
        // Keep matrix/text preview lines untouched.
        if s.starts_with("  ") || s.starts_with("preview") || s.starts_with("stats") || s.starts_with("ERROR") {
            lines.push(Line::from(Span::raw(s.clone())));
            continue;
        }

        // Prefer "key = value" formatting (we generate it in preview_selected/renderers).
        if let Some((k, v)) = s.split_once(" = ") {
            lines.push(Line::from(vec![
                Span::styled(k.to_string(), Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)),
                Span::raw(" = "),
                Span::styled(v.to_string(), Style::default().fg(Color::Green)),
            ]));
        } else if let Some((k, v)) = s.split_once('=') {
            // Backward compatibility for any remaining "key=value" lines.
            lines.push(Line::from(vec![
                Span::styled(k.to_string(), Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)),
                Span::raw(" = "),
                Span::styled(v.trim().to_string(), Style::default().fg(Color::Green)),
            ]));
        } else {
            // Fallback: raw line
            lines.push(Line::from(Span::raw(s.clone())));
        }
    }

    let inner_h = area.height.saturating_sub(2);
    let scroll = clamp_scroll(state.preview_scroll, lines.len(), inner_h);

    let paragraph = Paragraph::new(lines)
        .block(block)
        .wrap(Wrap { trim: false })
        .scroll((scroll, 0));
    f.render_widget(paragraph, area);
}

/// Render the value preview into lines (reusing your existing decoding logic)
fn render_value_preview(v: &GbfValue, max_elems: usize, rows: usize, cols: usize, stats: bool) -> Vec<String> {
    // Keep it simple: mirror the same output as print_value_preview but as Vec<String>.
    // We intentionally do not rely on stdout capturing.
    let mut out: Vec<String> = vec![];

    match v {
        GbfValue::Struct(m) => {
            out.push(format!("struct: {} field(s)", m.len()));
            let mut keys: Vec<&String> = m.keys().collect();
            keys.sort();
            for k in keys {
                out.push(format!("  {}", k));
            }
        }
        GbfValue::Numeric(n) => {
            out.extend(render_numeric_preview(n, max_elems, rows, cols, stats));
        }
        GbfValue::Logical(l) => {
            let numel = shape_numel(&l.shape);
            out.push(format!(
                "logical: shape={} numel={}",
                fmt_shape_usize(&l.shape),
                numel
            ));
            let show = max_elems.min(l.data.len());
            out.push(format!("preview (first {}): {:?}", show, &l.data[..show]));
        }
        GbfValue::String(s) => {
            let numel = shape_numel(&s.shape);
            out.push(format!(
                "string: shape={} numel={}",
                fmt_shape_usize(&s.shape),
                numel
            ));
            let show = max_elems.min(s.data.len());
            for i in 0..show {
                match &s.data[i] {
                    Some(x) => out.push(format!("  [{}] \"{}\"", i, x)),
                    None => out.push(format!("  [{}] <missing>", i)),
                }
            }
        }
        GbfValue::Char(c) => {
            let s = chararray_to_display(c);
            out.push(format!(
                "char: shape={} len={} text=\"{}\"",
                fmt_shape_usize(&c.shape),
                c.data.len(),
                s
            ));
        }
        GbfValue::DateTime(dt) => out.push(format!("datetime: {:?}", dt)),
        GbfValue::Duration(d) => out.push(format!("duration: {:?}", d)),
        GbfValue::CalendarDuration(cd) => out.push(format!("calendarDuration: {:?}", cd)),
        GbfValue::Categorical(cat) => {
            out.push(format!(
                "categorical: shape={} categories={} codes={}",
                fmt_shape_usize(&cat.shape),
                cat.categories.len(),
                cat.codes.len()
            ));
            let show = max_elems.min(cat.codes.len());
            for i in 0..show {
                let code = cat.codes[i];
                if code == 0 {
                    out.push(format!("  [{}] <undefined>", i));
                } else {
                    let idx = (code as usize).saturating_sub(1);
                    let label = cat
                        .categories
                        .get(idx)
                        .map(|s| s.as_str())
                        .unwrap_or("<?>");
                    out.push(format!("  [{}] {} => {}", i, code, label));
                }
            }
        }
        other => out.push(format!("{:?}", other)),
    }

    out
}

fn render_numeric_preview(n: &NumericArray, max_elems: usize, rows: usize, cols: usize, stats: bool) -> Vec<String> {
    let mut out = vec![];

    let class_key = numeric_class_key(&n.class);
    let elem_size = bytes_per_elem(&class_key).unwrap_or(1);
    let shape = &n.shape;
    let numel = shape_numel(shape);

    out.push(format!(
        "numeric: class={} complex={} shape={} numel={} bytes(real)={}",
        class_key,
        n.complex,
        fmt_shape_usize(shape),
        numel,
        n.real_le.len()
    ));

    if shape.len() == 2
        && !n.complex
        && (class_key == "double"
            || class_key == "single"
            || class_key == "int32"
            || class_key == "uint64")
    {
        let r_total = shape[0];
        let c_total = shape[1];
        let r_show = rows.min(r_total);
        let c_show = cols.min(c_total);

        out.push(format!("preview (top-left {}x{}):", r_show, c_show));

        for r in 0..r_show {
            let mut row = Vec::with_capacity(c_show);
            for c in 0..c_show {
                let idx = r + c * r_total;
                let off = idx * elem_size;
                if off + elem_size <= n.real_le.len() {
                    row.push(decode_scalar_to_string(
                        &class_key,
                        &n.real_le[off..off + elem_size],
                    ));
                } else {
                    row.push("?".into());
                }
            }
            out.push(format!("  {}", row.iter().map(|x| format!("{:>14}", x)).collect::<Vec<_>>().join(" ")));
        }
    } else if shape.len() == 3
        && !n.complex
        && (class_key == "double"
            || class_key == "single"
            || class_key == "int32"
            || class_key == "uint64")
    {
        let r_total = shape[0];
        let c_total = shape[1];
        let k_total = shape[2];

        let k_show = if r_total == 3 && c_total == 3 {
            6.min(k_total)
        } else {
            5.min(k_total)
        };

        let r_show = rows.min(r_total);
        let c_show = cols.min(c_total);

        out.push(format!("preview (top-left {}x{}x{}):", r_show, c_show, k_show));

        for k in 0..k_show {
            out.push(format!("slice [{}]", k));

            for r in 0..r_show {
                let mut row = Vec::with_capacity(c_show);
                for c in 0..c_show {
                    let idx = r + c * r_total + k * r_total * c_total;
                    let off = idx * elem_size;
                    if off + elem_size <= n.real_le.len() {
                        row.push(decode_scalar_to_string(
                            &class_key,
                            &n.real_le[off..off + elem_size],
                        ));
                    } else {
                        row.push("?".into());
                    }
                }
                out.push(format!(
                    "  {}",
                    row.iter()
                        .map(|x| format!("{:>14}", x))
                        .collect::<Vec<_>>()
                        .join(" ")
                ));
            }

            out.push("".to_string());
        }
    } else {
        let show = max_elems.min(numel);
        out.push(format!("preview (first {} elements):", show));

        if show <= 100 {
            for i in 0..show {
                let off = i * elem_size;
                if off + elem_size <= n.real_le.len() {
                    out.push(format!(
                        "  [{:>4}] {}",
                        i,
                        decode_scalar_to_string(&class_key, &n.real_le[off..off + elem_size])
                    ));
                } else {
                    out.push(format!("  [{:>4}] ?", i));
                }
            }
        } else {
            let mut line = String::new();
            for i in 0..show {
                let off = i * elem_size;
                if off + elem_size <= n.real_le.len() {
                    line.push_str(&decode_scalar_to_string(
                        &class_key,
                        &n.real_le[off..off + elem_size],
                    ));
                } else {
                    line.push('?');
                }
                line.push(' ');

                if (i + 1) % 10 == 0 {
                    out.push(format!("  {}", line.trim_end()));
                    line.clear();
                }
            }
            if !line.is_empty() {
                out.push(format!("  {}", line.trim_end()));
            }
        }
    }

    if stats && (class_key == "double" || class_key == "single") && !n.complex {
        let mut count = 0u64;
        let mut nan = 0u64;
        let mut inf = 0u64;
        let mut min = f64::INFINITY;
        let mut max = f64::NEG_INFINITY;
        let mut sum = 0.0f64;

        let step = elem_size;
        let mut i = 0usize;
        while i + step <= n.real_le.len() {
            let v = match class_key.as_str() {
                "double" => {
                    let mut a = [0u8; 8];
                    a.copy_from_slice(&n.real_le[i..i + 8]);
                    f64::from_le_bytes(a)
                }
                "single" => {
                    let mut a = [0u8; 4];
                    a.copy_from_slice(&n.real_le[i..i + 4]);
                    f32::from_le_bytes(a) as f64
                }
                _ => 0.0,
            };

            if v.is_nan() {
                nan += 1;
            } else if v.is_infinite() {
                inf += 1;
                if v.is_sign_positive() {
                    max = max.max(v);
                } else {
                    min = min.min(v);
                }
            } else {
                count += 1;
                sum += v;
                min = min.min(v);
                max = max.max(v);
            }

            i += step;
        }

        let mean = if count > 0 { sum / (count as f64) } else { f64::NAN };
        out.push(format!(
            "stats (full): count_finite={} nan={} inf={} min={} max={} mean={}",
            count, nan, inf, min, max, mean
        ));
    }

    out
}