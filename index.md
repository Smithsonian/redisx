---
excerpt: RedisX is a light-weight Redis client library for C/C++.
---

<img src="/redisx/resources/Sigmyne-logo-200x44.png" alt="Sigmyne logo" width="200" height="44" align="right"><br clear="all">

<img src="https://img.shields.io/github/v/release/Sigmyne/redisx?label=github" class="badge" alt="GitHub release version" align="left">
<img src="https://img.shields.io/fedora/v/redisx?color=lightblue" class="badge" alt="Fedora package version" align="left">
<br clear="all">

__RedisX__ is a light-weight [Redis](https://redis.io) client for C/C++. As such, it should also work with Redis forks 
/ clones like [Dragonfly](https://dragonfly.io) or [Valkey](https://valkey.io). It supports both interactive and 
pipelined Redis queries, managing and processing subscriptions. It also supports atomic execution blocks and LUA 
scripts loading. It can be used with one or more distinct Redis servers simultaneously.

While there are other C/C++ Redis clients available, this one is C90 compatible, and hence can be used on older 
platforms also. It is also small and fast, but still capable and versatile.

The __RedisX__ library was created, and is maintained, by Attila Kovács (Sigmyne, LLC), and it is available through 
the [Sigmyne/redisx](https://github.com/Sigmyne/redisx) repository on GitHub. 

This site contains various online resources that support the library:

__Downloads__

 - [Releases](https://github.com/Sigmyne/redisx/releases) from GitHub

__Documentation__

 - [User's guide](doc/README.md) (`README.md`)
 - [API Documentation](apidoc/html/files.html)
 - [History of changes](doc/CHANGELOG.md) (`CHANGELOG.md`)
 - [Issues](https://github.com/Sigmyne/redisx/issues) affecting __RedisX__ releases (past and/or present)
 - [Community Forum](https://github.com/Sigmyne/redisx/discussions) &ndash; ask a question, provide feedback, or 
   check announcements.

__Dependencies__

 - [Sigmyne/xchange](https://github.com/Sigmyne/xchange) -- structured data exchange framework
 
__Linux Packages__

RedisX also comes in packaged form for Fedora / EPEL based Linux, with the following packages

 - `redisx` -- Core library and `redisx-cli` tool.
 - `redisx-devel` -- C development files (headers and unversioned `.so` libraries)
 - `redisx-doc` -- HTML documentation
 
 


