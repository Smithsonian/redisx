/*
 @licstart  The following is the entire license notice for the JavaScript code in this file.

 The MIT License (MIT)

 Copyright (C) 1997-2020 by Dimitri van Heesch

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 @licend  The above is the entire license notice for the JavaScript code in this file
*/
var NAVTREE =
[
  [ "RedisX", "index.html", [
    [ "Changelog", "../../xchange/apidoc/html/md_CHANGELOG.html", [
      [ "Table of Contents", "index.html#autotoc_md8", null ],
      [ "Introduction", "index.html#autotoc_md9", [
        [ "A simple example", "index.html#autotoc_md10", null ],
        [ "Features overview", "index.html#autotoc_md11", [
          [ "General Features", "index.html#autotoc_md12", null ],
          [ "Redis / Valkey Features", "index.html#autotoc_md13", null ]
        ] ],
        [ "Related links", "index.html#autotoc_md14", null ]
      ] ],
      [ "Prerequisites", "index.html#autotoc_md16", null ],
      [ "Building RedisX", "index.html#autotoc_md18", null ],
      [ "Command-line interface (<tt>redisx-cli</tt>)", "index.html#autotoc_md20", null ],
      [ "Linking your application against RedisX", "index.html#autotoc_md22", null ],
      [ "Managing Redis server connections", "index.html#autotoc_md24", [
        [ "Initializing", "index.html#autotoc_md25", [
          [ "Sentinel", "index.html#autotoc_md26", null ]
        ] ],
        [ "Configuring", "index.html#autotoc_md27", [
          [ "TLS configuration", "index.html#autotoc_md28", null ],
          [ "Socket-level configuration", "index.html#autotoc_md29", null ],
          [ "Connection & disconnection hooks", "index.html#autotoc_md30", null ]
        ] ],
        [ "Connecting", "index.html#autotoc_md31", null ],
        [ "Disconnecting", "index.html#autotoc_md32", null ],
        [ "Reconnecting", "index.html#autotoc_md33", null ]
      ] ],
      [ "Simple Redis queries", "index.html#autotoc_md35", [
        [ "Interactive transactions", "index.html#autotoc_md36", null ],
        [ "Bundled Attributes", "index.html#autotoc_md37", null ],
        [ "Push notifications", "index.html#autotoc_md38", null ],
        [ "RESP data type", "index.html#autotoc_md39", null ]
      ] ],
      [ "Accessing key / value data", "index.html#autotoc_md41", [
        [ "Getting and setting keyed values", "index.html#autotoc_md42", null ],
        [ "Listing and Scanning", "index.html#autotoc_md43", null ]
      ] ],
      [ "Publish / subscribe (PUB/SUB) support", "index.html#autotoc_md45", [
        [ "Broadcasting messages", "index.html#autotoc_md46", null ],
        [ "Subscriptions", "index.html#autotoc_md47", null ]
      ] ],
      [ "Atomic execution blocks and LUA scripts", "index.html#autotoc_md49", [
        [ "Execution blocks", "index.html#autotoc_md50", null ],
        [ "LUA script loading and execution", "index.html#autotoc_md51", null ],
        [ "Custom Redis functions", "index.html#autotoc_md52", null ]
      ] ],
      [ "Advanced queries and pipelining", "index.html#autotoc_md54", [
        [ "Asynchronous client processing", "index.html#autotoc_md55", null ],
        [ "Bundled Attributes", "index.html#autotoc_md56", null ],
        [ "Pipelined transactions", "index.html#autotoc_md57", null ]
      ] ],
      [ "Redis clusters", "index.html#autotoc_md59", [
        [ "Cluster basics", "index.html#autotoc_md60", null ],
        [ "Detecting cluster reconfiguration", "index.html#autotoc_md61", null ],
        [ "Manual connection management", "index.html#autotoc_md62", null ]
      ] ],
      [ "Error handling", "index.html#autotoc_md64", [
        [ "Socket-level errors", "index.html#autotoc_md65", null ]
      ] ],
      [ "Debug support", "index.html#autotoc_md67", null ],
      [ "Future plans", "index.html#autotoc_md69", null ],
      [ "Release schedule", "index.html#autotoc_md71", null ],
      [ "[1.0.2] - 2025-11-17", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md1", [
        [ "Changed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md2", null ]
      ] ],
      [ "[1.0.1] - 2025-08-01", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md3", [
        [ "Fixed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md4", null ],
        [ "Changed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md5", null ]
      ] ],
      [ "[1.0.0] - 2025-05-06", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md6", null ]
    ] ],
    [ "Contributing to RedisX", "../../xchange/apidoc/html/md_CONTRIBUTING.html", null ],
    [ "Deprecated List", "deprecated.html", null ],
    [ "Data Structures", "annotated.html", [
      [ "Data Structures", "annotated.html", "annotated_dup" ],
      [ "Data Structure Index", "classes.html", null ],
      [ "Data Fields", "functions.html", [
        [ "All", "functions.html", null ],
        [ "Variables", "functions_vars.html", null ]
      ] ]
    ] ],
    [ "Files", "files.html", [
      [ "File List", "files.html", "files_dup" ],
      [ "Globals", "globals.html", [
        [ "All", "globals.html", "globals_dup" ],
        [ "Functions", "globals_func.html", "globals_func" ],
        [ "Typedefs", "globals_type.html", null ],
        [ "Enumerations", "globals_enum.html", null ],
        [ "Enumerator", "globals_eval.html", null ],
        [ "Macros", "globals_defs.html", null ]
      ] ]
    ] ]
  ] ]
];

var NAVTREEINDEX =
[
"annotated.html",
"redisx_8h.html#a4b5674d08da07250c0ca6116a4c48ccf"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';