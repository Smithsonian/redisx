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
      [ "Table of Contents", "index.html#autotoc_md11", null ],
      [ "Introduction", "index.html#autotoc_md12", [
        [ "A simple example", "index.html#autotoc_md13", null ],
        [ "Features overview", "index.html#autotoc_md14", [
          [ "General Features", "index.html#autotoc_md15", null ],
          [ "Redis / Valkey Features", "index.html#autotoc_md16", null ]
        ] ],
        [ "Related links", "index.html#autotoc_md17", null ]
      ] ],
      [ "Prerequisites", "index.html#autotoc_md19", null ],
      [ "Building RedisX", "index.html#autotoc_md21", null ],
      [ "Command-line interface (<tt>redisx-cli</tt>)", "index.html#autotoc_md23", null ],
      [ "Linking your application against RedisX", "index.html#autotoc_md25", null ],
      [ "Managing Redis server connections", "index.html#autotoc_md27", [
        [ "Initializing", "index.html#autotoc_md28", [
          [ "Sentinel", "index.html#autotoc_md29", null ]
        ] ],
        [ "Configuring", "index.html#autotoc_md30", [
          [ "TLS configuration", "index.html#autotoc_md31", null ],
          [ "Socket-level configuration", "index.html#autotoc_md32", null ],
          [ "Connection & disconnection hooks", "index.html#autotoc_md33", null ]
        ] ],
        [ "Connecting", "index.html#autotoc_md34", null ],
        [ "Disconnecting", "index.html#autotoc_md35", null ],
        [ "Reconnecting", "index.html#autotoc_md36", null ]
      ] ],
      [ "Simple Redis queries", "index.html#autotoc_md38", [
        [ "Interactive transactions", "index.html#autotoc_md39", null ],
        [ "Bundled Attributes", "index.html#autotoc_md40", null ],
        [ "Push notifications", "index.html#autotoc_md41", null ],
        [ "RESP data type", "index.html#autotoc_md42", null ]
      ] ],
      [ "Accessing key / value data", "index.html#autotoc_md44", [
        [ "Getting and setting keyed values", "index.html#autotoc_md45", null ],
        [ "Listing and Scanning", "index.html#autotoc_md46", null ]
      ] ],
      [ "Publish / subscribe (PUB/SUB) support", "index.html#autotoc_md48", [
        [ "Broadcasting messages", "index.html#autotoc_md49", null ],
        [ "Subscriptions", "index.html#autotoc_md50", null ]
      ] ],
      [ "Atomic execution blocks and LUA scripts", "index.html#autotoc_md52", [
        [ "Execution blocks", "index.html#autotoc_md53", null ],
        [ "LUA script loading and execution", "index.html#autotoc_md54", null ],
        [ "Custom Redis functions", "index.html#autotoc_md55", null ]
      ] ],
      [ "Advanced queries and pipelining", "index.html#autotoc_md57", [
        [ "Asynchronous client processing", "index.html#autotoc_md58", null ],
        [ "Bundled Attributes", "index.html#autotoc_md59", null ],
        [ "Pipelined transactions", "index.html#autotoc_md60", null ]
      ] ],
      [ "Redis clusters", "index.html#autotoc_md62", [
        [ "Cluster basics", "index.html#autotoc_md63", null ],
        [ "Detecting cluster reconfiguration", "index.html#autotoc_md64", null ],
        [ "Manual connection management", "index.html#autotoc_md65", null ]
      ] ],
      [ "Error handling", "index.html#autotoc_md67", [
        [ "Socket-level errors", "index.html#autotoc_md68", null ]
      ] ],
      [ "Debug support", "index.html#autotoc_md70", null ],
      [ "Future plans", "index.html#autotoc_md72", null ],
      [ "Release schedule", "index.html#autotoc_md74", null ],
      [ "[1.0.3] - 2026-02-16", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md1", [
        [ "Fixed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md2", null ],
        [ "Changed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md3", null ]
      ] ],
      [ "[1.0.2] - 2025-11-17", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md4", [
        [ "Changed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md5", null ]
      ] ],
      [ "[1.0.1] - 2025-08-01", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md6", [
        [ "Fixed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md7", null ],
        [ "Changed", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md8", null ]
      ] ],
      [ "[1.0.0] - 2025-05-06", "../../xchange/apidoc/html/md_CHANGELOG.html#autotoc_md9", null ]
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
"redisx_8h.html#a484532a0b208bbd9c7a6cb9663361696"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';