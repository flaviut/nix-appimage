const std = @import("std");
const ldso = @import("lib.zig");

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    const parsed_paths = try ldso.parseLdSoConf(allocator, "/etc/ld.so.conf");
    defer ldso.freePathList(allocator, parsed_paths);

    const env_ld = std.process.getEnvVarOwned(allocator, "LD_LIBRARY_PATH") catch |err| switch (err) {
        error.EnvironmentVariableNotFound => null,
        else => return err,
    };
    defer if (env_ld) |value| allocator.free(value);

    const parsed_const: []const []const u8 = parsed_paths;
    const combined = try ldso.extendLdLibraryPath(allocator, env_ld, parsed_const);
    defer allocator.free(combined);

    try std.io.getStdOut().writer().print("{s}\n", .{combined});
}
