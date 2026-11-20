#include "mls-unit-test/unittest.h"
#include "virtualfs.h"

using fakeos::VirtualFs;
using Error = VirtualFs::Error;

namespace {
VirtualFs world() {
    VirtualFs fs;
    fs.putDirectory("/home/guest", "guest", 0755, 0);
    fs.put("/etc/passwd", "root:x:0:0\n", "root", 0644, 0);
    fs.put("/home/guest/a.txt", "hello\n", "guest", 0644, 0);
    return fs;
}
} // namespace

TEST_SUIT_BEGIN(FakeosVirtualFs)

TEST_CASE("normalize handles dots, slashes and the root") {
    EXPECT(VirtualFs::normalize("/home/guest", "a/b") == "/home/guest/a/b");
    EXPECT(VirtualFs::normalize("/home/guest", "../x//y/./") == "/home/x/y");
    EXPECT(VirtualFs::normalize("/home", "/") == "/");
    EXPECT(VirtualFs::normalize("/", "../../..") == "/");
    EXPECT(VirtualFs::normalize("/a", "") == "/a");
    EXPECT(VirtualFs::parentOf("/a/b") == "/a");
    EXPECT(VirtualFs::parentOf("/a") == "/");
    EXPECT(VirtualFs::nameOf("/a/b") == "b");
    EXPECT(VirtualFs::join("/", "x") == "/x");
    EXPECT(VirtualFs::join("/a", "x") == "/a/x");
}

TEST_CASE("find, list and files") {
    auto fs = world();
    EXPECT(fs.find("/home/guest/a.txt")->data == "hello\n");
    EXPECT(fs.find("/home/guest/missing") == nullptr);
    EXPECT(fs.find("/home/guest/a.txt/x") == nullptr);
    EXPECT(fs.find("/")->directory);
    const auto listed = fs.list("/home/guest");
    EXPECT_EQ(listed.size(), std::size_t(1));
    EXPECT(listed[0].first == "a.txt");
}

TEST_CASE("Writing, appending and permissions") {
    auto fs = world();
    EXPECT(fs.write("/home/guest/b.txt", "x", "guest", 5) == Error::None);
    EXPECT(fs.write("/home/guest/b.txt", "y", "guest", 6, true) == Error::None);
    EXPECT(fs.find("/home/guest/b.txt")->data == "xy");
    EXPECT(fs.find("/home/guest/b.txt")->mtime == 6);
    EXPECT(fs.write("/etc/passwd", "z", "guest", 1) == Error::Permission);
    EXPECT(fs.write("/etc/new", "z", "guest", 1) == Error::Permission);
    EXPECT(fs.write("/etc/passwd", "z", "root", 1) == Error::None);
    EXPECT(fs.write("/home/guest", "z", "guest", 1) == Error::IsDirectory);
    EXPECT(fs.write("/nowhere/x", "z", "root", 1) == Error::NotFound);
    EXPECT(fs.write("/home/guest/a.txt/x", "z", "root", 1) ==
           Error::NotDirectory);
}

TEST_CASE("mkdir, parents and touch") {
    auto fs = world();
    EXPECT(fs.mkdir("/home/guest/d", "guest", 1) == Error::None);
    EXPECT(fs.mkdir("/home/guest/d", "guest", 1) == Error::Exists);
    EXPECT(fs.mkdir("/home/guest/x/y", "guest", 1) == Error::NotFound);
    EXPECT(fs.mkdir("/home/guest/x/y/z", "guest", 1, true) == Error::None);
    EXPECT(fs.find("/home/guest/x/y/z")->directory);
    EXPECT(fs.mkdir("/home/guest/a.txt/q", "guest", 1, true) ==
           Error::NotDirectory);
    EXPECT(fs.touch("/home/guest/t", "guest", 9) == Error::None);
    EXPECT(fs.touch("/home/guest/t", "guest", 10) == Error::None);
    EXPECT(fs.find("/home/guest/t")->mtime == 10);
    EXPECT(fs.mkdir("/etc/d", "guest", 1) == Error::Permission);
}

TEST_CASE("remove needs -r for directories and respects the parent") {
    auto fs = world();
    fs.mkdir("/home/guest/d", "guest", 1);
    fs.write("/home/guest/d/f", "abc", "guest", 1);
    const auto before = fs.nodeCount();
    EXPECT(fs.remove("/home/guest/d", "guest", false) == Error::NotEmpty);
    EXPECT(fs.remove("/home/guest/d", "guest", true) == Error::None);
    EXPECT_EQ(fs.nodeCount(), before - 2);
    EXPECT(fs.find("/home/guest/d") == nullptr);
    EXPECT(fs.remove("/etc/passwd", "guest", false) == Error::Permission);
    EXPECT(fs.remove("/nope", "guest", false) == Error::NotFound);
    EXPECT(fs.remove("/", "root", true) == Error::Invalid);
}

TEST_CASE("copy and move, also of directories") {
    auto fs = world();
    EXPECT(
        fs.copy("/home/guest/a.txt", "/home/guest/c.txt", "guest", 3, false) ==
        Error::None);
    EXPECT(fs.find("/home/guest/c.txt")->data == "hello\n");
    EXPECT(
        fs.copy("/home/guest/a.txt", "/home/guest/c.txt", "guest", 3, false) ==
        Error::Exists);
    fs.mkdir("/home/guest/d", "guest", 1);
    fs.write("/home/guest/d/f", "1", "guest", 1);
    EXPECT(fs.copy("/home/guest/d", "/home/guest/e", "guest", 3, false) ==
           Error::IsDirectory);
    EXPECT(fs.copy("/home/guest/d", "/home/guest/e", "guest", 3, true) ==
           Error::None);
    EXPECT(fs.find("/home/guest/e/f")->data == "1");
    EXPECT(fs.copy("/home/guest/d", "/home/guest/d/inner", "guest", 3, true) ==
           Error::Invalid);
    const auto nodes = fs.nodeCount();
    EXPECT(fs.move("/home/guest/e", "/home/guest/m", "guest", 4) ==
           Error::None);
    EXPECT(fs.find("/home/guest/e") == nullptr);
    EXPECT(fs.find("/home/guest/m/f")->data == "1");
    EXPECT_EQ(fs.nodeCount(), nodes);
    EXPECT(fs.move("/home/guest/m", "/home/guest/m/x", "guest", 4) ==
           Error::Invalid);
    EXPECT(fs.move("/etc/passwd", "/home/guest/p", "guest", 4) ==
           Error::Permission);
}

TEST_CASE("Limits stop runaway growth") {
    VirtualFs::Limits limits;
    limits.maxNodes = 5;
    limits.maxTotalBytes = 100;
    limits.maxFileBytes = 60;
    limits.maxDepth = 3;
    limits.maxNameBytes = 8;
    VirtualFs fs(limits);
    fs.putDirectory("/h", "guest", 0755, 0);
    EXPECT(fs.write("/h/big", std::string(61, 'x'), "guest", 1) ==
           Error::TooBig);
    EXPECT(fs.write("/h/a", std::string(60, 'x'), "guest", 1) == Error::None);
    EXPECT(fs.write("/h/b", std::string(41, 'x'), "guest", 1) ==
           Error::NoSpace);
    EXPECT(fs.write("/h/a", std::string(10, 'x'), "guest", 1, true) ==
           Error::TooBig);
    EXPECT(fs.write("/h/a", std::string(20, 'x'), "guest", 1) == Error::None);
    EXPECT(fs.write("/h/b", std::string(40, 'x'), "guest", 1) == Error::None);
    EXPECT(fs.touch("/h/c", "guest", 1) == Error::None); // The fifth node.
    EXPECT(fs.touch("/h/d", "guest", 1) == Error::NoSpace);
    fs.remove("/h/c", "guest", false);
    EXPECT(fs.mkdir("/h/toolongname", "guest", 1) == Error::Invalid);
    EXPECT(fs.mkdir("/h/x/y/z", "guest", 1) == Error::Invalid);
    fs.remove("/h/a", "guest", false);
    EXPECT_EQ(fs.totalBytes(), std::size_t(40));
}

TEST_CASE("chmod and read permission checks") {
    auto fs = world();
    EXPECT(fs.chmod("/etc/passwd", "guest", 0600) == Error::Permission);
    EXPECT(fs.chmod("/home/guest/a.txt", "guest", 0600) == Error::None);
    const auto *node = fs.find("/home/guest/a.txt");
    EXPECT(fs.canRead(*node, "guest"));
    EXPECT(!fs.canRead(*node, "other"));
    EXPECT(fs.canRead(*node, "root"));
}

TEST_SUIT_END
