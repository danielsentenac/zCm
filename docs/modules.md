# API Modules

Use this page as the single entry point for API modules:
- \subpage api_high_level "High-Level API": Process-oriented helpers that expose config-driven runtime behavior.

- \subpage api_low_level "Low-Level API": Core messaging, registry, socket, and context primitives.

\defgroup zcm_high_level High-Level API
\brief Process-oriented helpers that expose config-driven runtime behavior.
\details This module is intended for application code that wants to launch and
manage `zcm_proc` style services with minimal boilerplate.

\defgroup zcm_low_level Low-Level API
\brief Core messaging, registry, socket, and context primitives.
\details This module is intended for code that needs direct control over
transport sockets, typed payload encoding, and broker/node interactions.
