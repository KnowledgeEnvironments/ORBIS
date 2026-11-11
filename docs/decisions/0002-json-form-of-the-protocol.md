# ADR 0002: JSON form of the protocol

Status: accepted, 2026-10-29

## Context
The listener is C++ and has no Transit library. The spec offers a JSON form.
## Decision
Transit-JSON is the default wire and log encoding. The JSON form is a Malli-driven projection: keyword keys and values as strings without the colon, uuid as string, instant as ISO-8601 UTC, bytes as base64, ref as id string, seq as number. Decoding uses malli.transform/json-transformer against the frame and op schemas (a :multi on :op), so enumerations come back as keywords by position.
## Consequences
One codec namespace serves both forms. The JSON Schema export (make schema-json) is the client's mirror.
