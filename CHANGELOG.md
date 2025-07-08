# Changelog

All notable changes to the [Smithsonian/redisx](https://github.com/Smithsonian/redisx) library will be 
documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to 
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).


## [1.0.1-rc2] - 2025-07-08

Changes for the upcoming bug fix release, expected around 15 August 2025.

### Fixed

 - IPv6 host name resolution.

### Changed

 - Sockets are now always initialized with `SO_LINGER` disabled. Previously that was the case only when a timeout 
   value was configured.

 - More consistent distinction between debug messages (i.e. error traces), verbose output, and warning messages.


## [1.0.0] - 2025-05-06

Initial public release.
