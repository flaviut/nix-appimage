const std = @import("std");

const max_line_bytes = 1024 * 1024;

fn matchesStar(name: []const u8, pattern: []const u8) bool {
    if (std.mem.indexOfScalar(u8, pattern, '*')) |idx| {
        const prefix = pattern[0..idx];
        const suffix = pattern[idx + 1 ..];
        return std.mem.startsWith(u8, name, prefix) and std.mem.endsWith(u8, name, suffix);
    }
    return std.mem.eql(u8, name, pattern);
}

fn canonicalizeOrCopy(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
    return std.fs.realpathAlloc(allocator, path) catch allocator.dupe(u8, path);
}

fn pushPath(paths: *std.ArrayList([]u8), allocator: std.mem.Allocator, path: []const u8) !void {
    try paths.append(try allocator.dupe(u8, path));
}

fn parseConf(
    allocator: std.mem.Allocator,
    path: []const u8,
    seen: *std.StringHashMap(void),
    collected: *std.ArrayList([]u8),
) anyerror!void {
    const canonical = try canonicalizeOrCopy(allocator, path);
    const gop = try seen.getOrPut(canonical);
    if (gop.found_existing) {
        allocator.free(canonical);
        return;
    }
    gop.key_ptr.* = canonical;
    gop.value_ptr.* = {};

    const parent = std.fs.path.dirname(canonical) orelse ".";

    var file = try std.fs.cwd().openFile(canonical, .{});
    defer file.close();

    var buffered = std.io.bufferedReader(file.reader());
    var reader = buffered.reader();

    while (true) {
        const maybe_line = try reader.readUntilDelimiterOrEofAlloc(allocator, '\n', max_line_bytes);
        if (maybe_line == null) break;
        const line = maybe_line.?;
        defer allocator.free(line);

        var content = line;
        if (std.mem.indexOfScalar(u8, content, '#')) |hash_idx| {
            content = content[0..hash_idx];
        }
        const trimmed = std.mem.trim(u8, content, " \t\r\n");
        if (trimmed.len == 0) continue;

        if (std.mem.startsWith(u8, trimmed, "include")) {
            const rest = std.mem.trim(u8, trimmed["include".len ..], " \t");
            if (rest.len == 0) continue;

            const include_path = if (std.fs.path.isAbsolute(rest))
                try allocator.dupe(u8, rest)
            else
                try std.fs.path.join(allocator, &[_][]const u8{ parent, rest });
            defer allocator.free(include_path);

            try expandInclude(allocator, include_path, seen, collected);
        } else {
            try pushPath(collected, allocator, trimmed);
        }
    }
}

fn expandInclude(
    allocator: std.mem.Allocator,
    pattern: []const u8,
    seen: *std.StringHashMap(void),
    collected: *std.ArrayList([]u8),
) anyerror!void {
    if (std.mem.indexOfScalar(u8, pattern, '*')) |_| {
        const parent = std.fs.path.dirname(pattern) orelse ".";
        const pattern_name = std.fs.path.basename(pattern);

        var dir = try std.fs.cwd().openDir(parent, .{ .iterate = true });
        defer dir.close();

        var files = std.ArrayList([]u8).init(allocator);
        defer {
            for (files.items) |item| allocator.free(item);
            files.deinit();
        }

        var it = dir.iterate();
        while (try it.next()) |entry| {
            if (entry.kind != .file) continue;
            if (!matchesStar(entry.name, pattern_name)) continue;

            const joined = try std.fs.path.join(allocator, &[_][]const u8{ parent, entry.name });
            try files.append(joined);
        }

        const Ctx = struct {};
        const lessThan = struct {
            pub fn lt(_: Ctx, a: []u8, b: []u8) bool {
                return std.mem.lessThan(u8, a, b);
            }
        };

        std.sort.insertion([]u8, files.items, Ctx{}, lessThan.lt);

        for (files.items) |file_path| {
            try parseConf(allocator, file_path, seen, collected);
        }
    } else {
        try parseConf(allocator, pattern, seen, collected);
    }
}

pub fn parseLdSoConf(allocator: std.mem.Allocator, path: []const u8) anyerror![][]u8 {
    var seen = std.StringHashMap(void).init(allocator);
    defer {
        var it = seen.iterator();
        while (it.next()) |entry| allocator.free(entry.key_ptr.*);
        seen.deinit();
    }

    var collected = std.ArrayList([]u8).init(allocator);
    errdefer {
        for (collected.items) |item| allocator.free(item);
        collected.deinit();
    }

    try parseConf(allocator, path, &seen, &collected);
    return collected.toOwnedSlice();
}

pub fn extendLdLibraryPath(
    allocator: std.mem.Allocator,
    current: ?[]const u8,
    parsed_paths: []const []const u8,
) ![]u8 {
    var entries = std.ArrayList([]u8).init(allocator);
    errdefer {
        for (entries.items) |item| allocator.free(item);
        entries.deinit();
    }

    if (current) |value| {
        var it = std.mem.splitScalar(u8, value, ':');
        while (it.next()) |segment| {
            if (segment.len == 0) continue;
            try entries.append(try allocator.dupe(u8, segment));
        }
    }

    const contains = struct {
        fn check(entries_slice: []const []const u8, candidate: []const u8) bool {
            for (entries_slice) |entry| {
                if (std.mem.eql(u8, entry, candidate)) return true;
            }
            return false;
        }
    }.check;

    for (parsed_paths) |path| {
        if (contains(entries.items, path)) continue;
        try entries.append(try allocator.dupe(u8, path));
    }

    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();

    for (entries.items, 0..) |entry, idx| {
        if (idx > 0) try out.append(':');
        try out.appendSlice(entry);
    }

    const result = try out.toOwnedSlice();

    for (entries.items) |entry| allocator.free(entry);
    entries.deinit();

    return result;
}

pub fn freePathList(allocator: std.mem.Allocator, paths: [][]u8) void {
    for (paths) |path| allocator.free(path);
    allocator.free(paths);
}

test "parses basic paths and skips comments" {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.writeFile("ld.so.conf",
        \\# comment
        \\/lib
        \\/usr/lib  # trailing comment
        \\
        \\/opt/lib
        \\
    );

    const conf_path = try std.fs.path.join(allocator, &[_][]const u8{ tmp.path, "ld.so.conf" });
    defer allocator.free(conf_path);

    const parsed = try parseLdSoConf(allocator, conf_path);
    defer freePathList(allocator, parsed);

    const expected = [_][]const u8{ "/lib", "/usr/lib", "/opt/lib" };
    try std.testing.expectEqual(expected.len, parsed.len);
    for (expected, 0..) |entry, idx| {
        try std.testing.expect(std.mem.eql(u8, entry, parsed[idx]));
    }
}

test "follows simple includes with glob" {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.makePath("ld.so.conf.d");
    try tmp.dir.writeFile("ld.so.conf", "include ld.so.conf.d/*.conf\n");
    try tmp.dir.writeFile("ld.so.conf.d/a.conf", "/a/lib\n");
    try tmp.dir.writeFile("ld.so.conf.d/b.conf", "/b/lib\n");
    try tmp.dir.writeFile("ld.so.conf.d/ignored.txt", "/should/not/read\n");

    const conf_path = try std.fs.path.join(allocator, &[_][]const u8{ tmp.path, "ld.so.conf" });
    defer allocator.free(conf_path);

    const parsed = try parseLdSoConf(allocator, conf_path);
    defer freePathList(allocator, parsed);

    const expected = [_][]const u8{ "/a/lib", "/b/lib" };
    try std.testing.expectEqual(expected.len, parsed.len);
    for (expected, 0..) |entry, idx| {
        try std.testing.expect(std.mem.eql(u8, entry, parsed[idx]));
    }
}

test "extends ld library path without duplicates" {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const parsed = [_][]const u8{ "/two", "/three" };
    const result = try extendLdLibraryPath(allocator, "/one:/two", &parsed);
    defer allocator.free(result);

    try std.testing.expect(std.mem.eql(u8, result, "/one:/two:/three"));
}
