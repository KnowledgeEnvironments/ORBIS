# Ethos

Why this exists, in the order the ideas arrived. The spec says what; this says why.

## Context is the scarce resource

Every token an agent reads costs money and, when irrelevant, costs quality. Today
context is assembled by code: retrievers, prompt templates, skills files, summarizers,
each its own system, none auditable. Here context is a property of the graph. What a
principal can name bounds what it can read; depth and salience shape the rest; a named
serialization caches it and replays it. The rules that set permissions set context cost.

## Richness is affordable because the agent does the setup

The Lisp machines, Smalltalk, and Apollo's Domain/AEGIS lost to Unix and files because their
richness had a price paid in human attention: name everything, declare types, write presentations, mount
namespaces, document inside the system, keep grants current. People will not do that, so
the lazy system won. An agent will do all of it, cheaply, and the product of the ceremony
is exactly what the agent needs to work: its own context. Setup produces instruction
fields, bindings, and memory, which improve the next resolution, which makes the next task
cheaper. The architecture can be as baroque as the work needs, because upkeep is the
agent's job and the knowledge of how to use the system travels with the objects
themselves. The world explains itself. Its first resident sets up itself.

## The world is the harness

There is no agent program outside the world. The loop that interprets, resolves,
serializes, evaluates, and acts is kernel code, one per process-driven principal, with
no state outside that principal's fields. Sessions are streams, tools are actions,
subagents are principals, channels are routes, cron is a trigger, memory is authored
fields. Everything a harness did is a named object with a record.

## People, agents, and appliances are one class

A person, a process, and an office appliance share one class, one namespace model, one
API; a flag says what drives each. The product a team builds is itself a principal, so
the team is observable: who touched what, under which grant, from which inputs, at what
cost. A pile of files cannot be observed at any tooling budget. A principal can.

## Naming is the mechanism

Everything has a name. A name resolves for a principal or it does not, and that one
predicate carries access, attention, cost, and audit. Names are location free; location
is a property of the object, held by the adapter that backs it. Move a thing and every
reference, memory field, and serialization that named it still resolves.

## The log is the truth

No image holds the system. Every object has a stream; present state is a fold over it; a
run is a value with a name. The single image is what killed the ancestors. Streams
federate; images do not.

## The listener makes it legible

The agent pays for richness in setup; the person is repaid only if the client renders the
record so that inspection is understanding. The world owns structure. The client owns
legibility, and a correct world that cannot be read has failed the person. Visualization
is first-order work, and the client is a serious piece of software, native and fast, with
text quality that respects a reader.

## Reviving the ancestors into the future

Mathematica's evaluating notebook, Genera's inspectable world with actionable references,
Smalltalk's change-from-inside, Palantir's shared domain model with provenance, Slurm's
explicit jobs, Max and LabVIEW's wired dataflow, the resident agent loop, durable memory.
Each piece was proven once by people with worse tools. Each is native here, without the
single image that killed the vessel it came in. The missing tenant has arrived: a
workforce of processes that needs the same names, the same record, and the same room as
the people it works beside.

## How we build it

No special cases for demos. Compose from the fourteen classes or write down why they are
not enough. The log is the only state. One access path. Names from the ontology. A gap
between spec and code is a finding, written down, never a silent divergence. From the
first green kernel-zero, the development agents work as principals inside the world they
are building.
