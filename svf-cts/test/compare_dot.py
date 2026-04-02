#!/usr/bin/env python3
"""
Structural comparison of PAG DOT files from LLVM and TreeSitter frontends.

This compares the *structure* of the PAG graphs, not the exact bytes.
The comparison checks:
  1. Edge types present (Addr, Copy, Load, Store, Gep, Call, Ret, Phi)
  2. Node type counts (ValVar, StackObjVar, HeapObjVar, GlobalObjVar, etc.)
  3. Function entry/exit node counts
  4. Structural patterns (e.g., for `p = &x`, both should have Addr edge)

What "match" means (not 100% identical):
  - NodeIDs will differ
  - LLVM may have extra nodes from lowering
  - Node labels differ (LLVM uses %1, %2; we use x, p)
  - The graph structure (types of edges) should match
"""

import re
import sys
import os


def parse_dot(filepath):
    """Parse a DOT file and extract nodes and edges with their types."""
    nodes = {}  # id -> {'label': str, 'type': str, 'shape': str}
    edges = []  # [{'src': str, 'dst': str, 'type': str, 'label': str}]

    with open(filepath, 'r') as f:
        content = f.read()

    # Parse line by line for robustness (labels may contain [] characters)
    for line in content.split('\n'):
        line = line.strip()

        # Edge: NodeXX -> NodeYY[color=green];
        edge_match = re.match(r'Node(\w+)\s*->\s*Node(\w+)\s*\[(.+)\]', line)
        if edge_match:
            src = edge_match.group(1)
            dst = edge_match.group(2)
            attrs = edge_match.group(3)

            label_match = re.search(r'label\s*=\s*"([^"]*)"', attrs)
            label = label_match.group(1) if label_match else ""

            color_match = re.search(r'color\s*=\s*(\w+)', attrs)
            color = color_match.group(1) if color_match else ""

            # SVF uses colors for edge types: green=Addr, red=Store, blue=Load,
            # black=Copy, grey=Copy, purple=Gep, orange=Call, cyan=Ret
            edge_type = classify_edge_by_color(color) if color else classify_edge(label)
            edges.append({
                'src': src,
                'dst': dst,
                'type': edge_type,
                'label': label,
                'color': color,
            })
            continue

        # Node: NodeXX [shape=record,...,label="..."];
        node_match = re.match(r'Node(\w+)\s*\[(.+)\]', line)
        if node_match:
            node_id = node_match.group(1)
            attrs = node_match.group(2)

            label_match = re.search(r'label\s*=\s*"((?:[^"\\]|\\.)*)"', attrs)
            shape_match = re.search(r'shape\s*=\s*(\w+)', attrs)

            label = label_match.group(1) if label_match else ""
            shape = shape_match.group(1) if shape_match else "box"

            # Determine node type from label
            node_type = classify_node(label)

            nodes[node_id] = {
                'label': label,
                'type': node_type,
                'shape': shape,
            }

    return nodes, edges


def classify_node(label):
    """Classify a PAG node by its label.

    Uses the explicit type name from SVF's DOT output (e.g., 'FunObjVar', 'StackObjVar').
    The label format is: {[func] TypeName ID: N\\ndetails}
    """
    # Extract the type name from the label - it appears after optional [funcname]
    # e.g., "{[main] FunObjVar ID: 5 ...}" or "{ConstNullPtrValVar ID: 0 ...}"
    type_match = re.search(r'(?:\[[\w]+\]\s*)?(\w+(?:Var|PN|Ptr))\s+ID:', label)
    if type_match:
        type_name = type_match.group(1)
        # Normalize known types
        type_map = {
            'FunObjVar': 'FunObjVar',
            'FunValVar': 'FunValVar',
            'GlobalObjVar': 'GlobalObjVar',
            'GlobalValVar': 'GlobalValVar',
            'StackObjVar': 'StackObjVar',
            'HeapObjVar': 'HeapObjVar',
            'ArgValVar': 'ArgValVar',
            'RetValPN': 'RetValPN',
            'ConstIntValVar': 'ConstIntValVar',
            'ConstFPValVar': 'ConstFPValVar',
            'ConstNullPtrValVar': 'ConstNullPtr',
            'DummyValVar': 'DummyValVar',
            'DummyObjVar': 'DummyObjVar',
            'BlackHoleVar': 'BlackHole',
            'ConstantObjVar': 'Constant',
            'ValVar': 'ValVar',
            'ObjVar': 'ObjVar',
        }
        if type_name in type_map:
            return type_map[type_name]
        # Fallback: check partial matches
        for key, val in type_map.items():
            if key.lower() in type_name.lower():
                return val

    # Fallback: keyword matching
    label_lower = label.lower()
    if 'funobjvar' in label_lower:
        return 'FunObjVar'
    if 'funvalvar' in label_lower:
        return 'FunValVar'
    if 'stackobj' in label_lower:
        return 'StackObjVar'
    if 'heapobj' in label_lower:
        return 'HeapObjVar'
    if 'retvalpn' in label_lower:
        return 'RetValPN'
    if 'dummyval' in label_lower:
        return 'DummyValVar'
    if 'dummyobj' in label_lower:
        return 'DummyObjVar'
    if 'constnullptr' in label_lower:
        return 'ConstNullPtr'
    if 'valvar' in label_lower:
        return 'ValVar'
    if 'objvar' in label_lower:
        return 'ObjVar'

    return 'Other'


def classify_edge(label):
    """Classify a PAG edge by its label."""
    label_lower = label.lower()

    if 'addr' in label_lower:
        return 'Addr'
    if 'copy' in label_lower:
        return 'Copy'
    if 'load' in label_lower:
        return 'Load'
    if 'store' in label_lower:
        return 'Store'
    if 'gep' in label_lower:
        return 'Gep'
    if 'call' in label_lower:
        return 'Call'
    if 'ret' in label_lower:
        return 'Ret'
    if 'phi' in label_lower:
        return 'Phi'
    if 'binary' in label_lower or 'binop' in label_lower:
        return 'BinaryOp'
    if 'branch' in label_lower or 'cmp' in label_lower:
        return 'Branch'
    if 'unary' in label_lower:
        return 'UnaryOp'

    return 'Other'


def classify_edge_by_color(color):
    """Classify a PAG edge by its DOT color (SVF's convention)."""
    color_map = {
        'green': 'Addr',
        'red': 'Store',
        'blue': 'Load',
        'black': 'Copy',
        'grey': 'Copy',
        'gray': 'Copy',
        'purple': 'Gep',
        'orange': 'Call',
        'cyan': 'Ret',
        'gold': 'Phi',
    }
    return color_map.get(color.lower(), 'Other')


def compare_graphs(llvm_nodes, llvm_edges, ts_nodes, ts_edges):
    """Compare two PAG graphs structurally. Returns (passed, report)."""
    report = []
    issues = []

    # 1. Compare edge type sets
    llvm_edge_types = set(e['type'] for e in llvm_edges)
    ts_edge_types = set(e['type'] for e in ts_edges)

    report.append("Edge types:")
    report.append(f"  LLVM: {sorted(llvm_edge_types)}")
    report.append(f"  TS:   {sorted(ts_edge_types)}")

    missing_edge_types = llvm_edge_types - ts_edge_types - {'Other'}
    extra_edge_types = ts_edge_types - llvm_edge_types - {'Other'}

    if missing_edge_types:
        issues.append(f"Missing edge types: {sorted(missing_edge_types)}")
    if extra_edge_types:
        report.append(f"  Extra edge types in TS (may be ok): {sorted(extra_edge_types)}")

    # 2. Compare edge type counts
    llvm_edge_counts = {}
    for e in llvm_edges:
        llvm_edge_counts[e['type']] = llvm_edge_counts.get(e['type'], 0) + 1
    ts_edge_counts = {}
    for e in ts_edges:
        ts_edge_counts[e['type']] = ts_edge_counts.get(e['type'], 0) + 1

    report.append("\nEdge type counts:")
    all_edge_types = sorted(set(llvm_edge_counts.keys()) | set(ts_edge_counts.keys()))
    for et in all_edge_types:
        lc = llvm_edge_counts.get(et, 0)
        tc = ts_edge_counts.get(et, 0)
        marker = "  " if et == 'Other' else ("OK" if tc >= lc else "!!")
        report.append(f"  {et:12s}: LLVM={lc:3d}  TS={tc:3d}  [{marker}]")

    # 3. Compare node type counts
    llvm_node_types = {}
    for n in llvm_nodes.values():
        llvm_node_types[n['type']] = llvm_node_types.get(n['type'], 0) + 1
    ts_node_types = {}
    for n in ts_nodes.values():
        ts_node_types[n['type']] = ts_node_types.get(n['type'], 0) + 1

    report.append("\nNode type counts:")
    all_node_types = sorted(set(llvm_node_types.keys()) | set(ts_node_types.keys()))
    for nt in all_node_types:
        lc = llvm_node_types.get(nt, 0)
        tc = ts_node_types.get(nt, 0)
        report.append(f"  {nt:18s}: LLVM={lc:3d}  TS={tc:3d}")

    # 4. Check key structural patterns
    report.append("\nStructural checks:")

    # Check: Addr edges exist (every local var should have one)
    llvm_addr = sum(1 for e in llvm_edges if e['type'] == 'Addr')
    ts_addr = sum(1 for e in ts_edges if e['type'] == 'Addr')
    if llvm_addr > 0 and ts_addr == 0:
        issues.append("No Addr edges in TS (expected >= 1)")
    report.append(f"  Addr edges: LLVM={llvm_addr} TS={ts_addr}")

    # Check: Store edges
    llvm_store = sum(1 for e in llvm_edges if e['type'] == 'Store')
    ts_store = sum(1 for e in ts_edges if e['type'] == 'Store')
    if llvm_store > 0 and ts_store == 0:
        issues.append("No Store edges in TS (expected >= 1)")
    report.append(f"  Store edges: LLVM={llvm_store} TS={ts_store}")

    # Check: Load edges
    llvm_load = sum(1 for e in llvm_edges if e['type'] == 'Load')
    ts_load = sum(1 for e in ts_edges if e['type'] == 'Load')
    if llvm_load > 0 and ts_load == 0:
        issues.append("No Load edges in TS (expected >= 1)")
    report.append(f"  Load edges: LLVM={llvm_load} TS={ts_load}")

    # Check: Total node count is in reasonable range
    llvm_total = len(llvm_nodes)
    ts_total = len(ts_nodes)
    if ts_total == 0:
        issues.append("No nodes in TS graph")
    elif llvm_total > 0:
        ratio = ts_total / llvm_total
        if ratio < 0.3:
            issues.append(f"TS has very few nodes ({ts_total} vs LLVM {llvm_total})")
        elif ratio > 5.0:
            issues.append(f"TS has too many nodes ({ts_total} vs LLVM {llvm_total})")
    report.append(f"  Total nodes: LLVM={llvm_total} TS={ts_total}")
    report.append(f"  Total edges: LLVM={len(llvm_edges)} TS={len(ts_edges)}")

    passed = len(issues) == 0

    if issues:
        report.append("\nISSUES:")
        for issue in issues:
            report.append(f"  - {issue}")
        report.append("\n[FAIL]")
    else:
        report.append("\n[PASS]")

    return passed, "\n".join(report)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <llvm.dot> <treesitter.dot>")
        sys.exit(1)

    llvm_dot = sys.argv[1]
    ts_dot = sys.argv[2]

    if not os.path.exists(llvm_dot):
        print(f"Error: LLVM DOT file not found: {llvm_dot}")
        sys.exit(1)
    if not os.path.exists(ts_dot):
        print(f"Error: TreeSitter DOT file not found: {ts_dot}")
        sys.exit(1)

    llvm_nodes, llvm_edges = parse_dot(llvm_dot)
    ts_nodes, ts_edges = parse_dot(ts_dot)

    passed, report = compare_graphs(llvm_nodes, llvm_edges, ts_nodes, ts_edges)
    print(report)

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
