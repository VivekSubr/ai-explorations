---
name: dropbox-project
description: 'Use when: working on the AI-generated Dropbox-like clone docs/specs; generating or updating HLD.md, CLIENT.md, BACKEND.md, SCALE.md, OpenAPI YAML, API workflows, backend design, client design, object-store design, scaling calculations, or autoscaling notes. Always load AGENCY.md context first.'
argument-hint: 'Describe the Dropbox clone doc/spec task'
---

# Dropbox Project Documentation Skill

Use this skill for documentation and specification work in the Dropbox-like object store project.

## Required Context Loading

Before changing or generating project docs, read `../../../AGENCY.md` and keep it in context. Treat it as the project map and source for what each checked-in Markdown file is responsible for.

Current project map from `AGENCY.md`:

1. `HLD.md` contains high-level requirements and design.
2. `BACKEND.md` contains backend implementation and architecture details.
3. `CLIENT.md` contains client workflows and frontend API-call behavior.
4. `SCALE.md` contains scaling calculations and autoscaling design.
5. Only Markdown design files are expected to be checked in for this AI-generated clone showcase.

If `AGENCY.md` is missing or conflicts with another instruction file, stop and ask the user which source should win.

## When To Use

Use this skill when the user asks to:

1. Generate or revise `HLD.md`, `BACKEND.md`, `CLIENT.md`, `SCALE.md`, or files under `openapi/`.
2. Convert design prose into an API spec, client spec, backend spec, scale plan, or implementation plan.
3. Check consistency between the HLD, client workflows, backend design, object-store behavior, and OpenAPI spec.
4. Add explicit assumptions, open questions, or design gaps for the Dropbox-like clone.

## Source Of Truth Order

Use this order when deriving one document from another:

1. `AGENCY.md` for project packaging and document ownership.
2. `HLD.md` for product goals, core requirements, system elements, and API behavior.
3. Existing target document for local style, terminology, and prior decisions.
4. `openapi/openapi.yaml` for concrete API shapes once it exists.
5. Other Markdown docs only for the subsystem they own.

When sources disagree, preserve the conflict as an open question instead of silently inventing a decision.

## Documentation Workflow

1. Read `../../../AGENCY.md` first.
2. Read the smallest relevant source document, usually `../../../HLD.md`.
3. Read the target document before editing it, even if it is empty.
4. State a local hypothesis about what the target document should contain.
5. Make a focused edit that updates only the requested document or spec.
6. Validate with the cheapest useful check:
   - Markdown: balanced fenced code blocks and editor diagnostics.
   - OpenAPI YAML: YAML parse plus an OpenAPI linter when available.
7. Report any open API or design gaps clearly in the final response.

## Style

Write concise design specs with concrete workflows, interfaces, responsibilities, and failure behavior. Prefer numbered flows, tables, and short sections where they make the design easier to scan.

Keep spelling and terminology consistent with the target document when possible, but fix obvious typos in newly generated content.

## Project-Specific Design Rules

1. The system is a scalable object store plus load balancer, similar to Dropbox.
2. Availability is prioritized over strict consistency.
3. Files may be as large as 50GB.
4. The client is Qt-based and rebuilt per platform.
5. Authentication uses Google SSO and bearer tokens on API calls.
6. File bytes are separate from metadata. The client uses metadata to decide display behavior.
7. Large uploads should use resumable upload sessions and 256KB-aligned chunks.
8. Large downloads should support redirects and resume behavior.
9. Sharing is a product goal, but the current HLD API does not yet define sharing endpoints.

## Expected Outputs

For Markdown specs, include:

1. Scope and goals.
2. Main components and responsibilities.
3. Important workflows.
4. API usage or contracts where relevant.
5. Failure handling and retry behavior where relevant.
6. Security, reliability, or scaling notes where relevant.
7. Open questions when the HLD does not define enough detail.

For OpenAPI specs, include:

1. OpenAPI 3.1 YAML.
2. Bearer auth.
3. Paths, parameters, responses, reusable schemas, and error responses.
4. Examples that conform to schemas.
5. Redirect and resumable upload responses when relevant.