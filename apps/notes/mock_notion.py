#!/usr/bin/env python3
"""Mock Notion API for testing the mdk notes importer (MDK_NOTION_API)."""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

PAGES = [
    {
        "object": "page", "id": "11111111-2222-3333-4444-555555555555",
        "created_time": "2026-11-01T09:00:00.000Z",
        "last_edited_time": "2026-11-12T09:00:00.000Z",
        "url": "https://notion.so/Page-One-11111111222233334444555555555555",
        "properties": {
            "Name": {"id": "t", "type": "title", "title": [
                {"type": "text", "text": {"content": "Page One"},
                 "annotations": {"bold": False, "italic": False, "strikethrough": False, "underline": False, "code": False, "color": "default"},
                 "plain_text": "Page One", "href": None}]},
            "Tags": {"id": "r", "type": "rich_text", "rich_text": [
                {"type": "text", "text": {"content": "x"},
                 "annotations": {"bold": False, "italic": False, "strikethrough": False, "underline": False, "code": False, "color": "default"},
                 "plain_text": "x", "href": None}]},
        },
    },
    {
        "object": "page", "id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        "created_time": "2026-10-15T08:00:00.000Z",
        "last_edited_time": "2026-10-20T08:00:00.000Z",
        "url": "https://notion.so/Page-Two-aaaaaaaabbbbccccddddeeeeeeeeeeee",
        "properties": {
            "Title": {"id": "t", "type": "title", "title": [
                {"type": "text", "text": {"content": "Page Two & <Co>"},
                 "annotations": {"bold": False, "italic": False, "strikethrough": False, "underline": False, "code": False, "color": "default"},
                 "plain_text": "Page Two & <Co>", "href": None}]},
        },
    },
]

def rt(text, bold=False, italic=False, strike=False, code=False, href=None):
    return {"type": "text", "text": {"content": text, "link": ({"url": href} if href else None)},
            "annotations": {"bold": bold, "italic": italic, "strikethrough": strike,
                            "underline": False, "code": code, "color": "default"},
            "plain_text": text, "href": href}

def blk(id_, type_, payload, has_children=False):
    return {"object": "block", "id": id_, "type": type_, "has_children": has_children, type_: payload}

BLOCKS = {
    # page 1: rich mix incl. nested children + a table
    "11111111-2222-3333-4444-555555555555": [
        blk("p1", "heading_1", {"rich_text": [rt("Heading One")], "color": "default"}),
        blk("p2", "paragraph", {"rich_text": [rt("Hello "), rt("bold", bold=True), rt(" and "),
            rt("italic", italic=True), rt(" and "), rt("code", code=True), rt(" plus "),
            rt("link", href="https://example.com")], "color": "default"}),
        blk("p3", "to_do", {"rich_text": [rt("Done thing")], "checked": True, "color": "default"}),
        blk("p4", "to_do", {"rich_text": [rt("Todo thing")], "checked": False, "color": "default"}),
        blk("p5", "bulleted_list_item", {"rich_text": [rt("item a")], "color": "default"}, has_children=True),
        blk("p6", "quote", {"rich_text": [rt("quoted wisdom")], "color": "default"}),
        blk("p7", "code", {"rich_text": [rt("fn main() {}\n")], "language": "rust", "color": "default"}),
        blk("p8", "divider", {}, False),
        blk("p9", "callout", {"rich_text": [rt("note this")], "icon": {"type": "emoji", "emoji": "💡"}, "color": "gray_background"}),
        blk("p10", "child_page", {"title": "Sub Page"}),
        blk("p11", "toggle", {"rich_text": [rt("toggle me")], "color": "default"}, has_children=True),
        blk("p12", "table", {}, has_children=True),
    ],
    # children of p5 (nested list under item a)
    "p5": [blk("p5a", "bulleted_list_item", {"rich_text": [rt("nested item")], "color": "default"})],
    # children of p11 (toggle content)
    "p11": [blk("p11a", "paragraph", {"rich_text": [rt("inside toggle")], "color": "default"})],
    # children of p12 (table rows)
    "p12": [
        blk("r1", "table_row", {"cells": [[[rt("Name")], [rt("Age")]]]}),
        blk("r2", "table_row", {"cells": [[[rt("Ann")], [rt("30")]]]}),
    ],
    # page 2: minimal
    "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee": [
        blk("q1", "heading_2", {"rich_text": [rt("Second page")]}),
        blk("q2", "paragraph", {"rich_text": [rt("short")]}),
    ],
}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _j(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        self.rfile.read(n)
        if self.path == "/v1/search":
            self._j({"object": "list", "results": PAGES, "has_more": False, "next_cursor": None})
        else:
            self._j({"error": "not found"}, 404)
    def do_GET(self):
        path = self.path.split("?")[0]
        if path.startswith("/v1/blocks/") and path.endswith("/children"):
            bid = path.split("/")[3]
            if "start_cursor" in self.path:
                self._j({"object": "list", "results": [], "has_more": False, "next_cursor": None})
            else:
                self._j({"object": "list", "results": BLOCKS.get(bid, []), "has_more": False, "next_cursor": None})
        else:
            self._j({"error": "not found"}, 404)

HTTPServer(("127.0.0.1", 8399), H).serve_forever()
