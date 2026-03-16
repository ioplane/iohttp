---
name: iohttp-documentation-style
description: Use when creating or editing project documentation in README.md, docs/, docs/plans/, AGENTS.md, or contributor-facing markdown files.
---

# iohttp Documentation Style

## Required Style

- English is authoritative. Write or revise `docs/en/*` first.
- Russian is a translation/adaptation layer. Update `docs/ru/*` only after the English version is stable.
- Use strict technical language.
- Write facts, interfaces, constraints, and examples.
- Do not write narrative introductions, project philosophy, or motivational text.
- Do not use marketing language such as "powerful", "better", "elegant", "modern" without a measurable criterion.
- Prefer tables, lists, API contracts, limits, and state descriptions.
- Use short examples only when they clarify behavior or ownership.
- Use Mermaid for diagrams. Do not add image-only diagrams when Mermaid is sufficient.
- Use extended GitHub Markdown when it improves scanability:
  - tables for contracts and limits
  - fenced code blocks with language tags
  - blockquote alerts (`> [!NOTE]`) only for strict warnings
  - `<details>` only for long secondary examples
- Every stable document must begin with badges.
- Badge links must point to official standards, official documentation, the official repository, or the project repository.
- Keep language consistent. Avoid mixed English/Russian sentences.
- Russian documentation must use Russian prose. Keep English only for API identifiers, macro names, external project names, protocol names, and RFC identifiers.

## Required Structure

- `docs/README.md` is the stable top-level index.
- `docs/en/README.md` and `docs/ru/README.md` are language indexes.
- Numbered documents in `docs/en/` and `docs/ru/` must stay in the same order and cover the same topics.
- `docs/plans/` contains sprint plans, ROADMAP.md, and BACKLOG.md.
- `docs/tmp/` is non-authoritative scratch space.

## Badges

- Use badges in every stable document.
- Preferred targets: ISO C standard page, RFC Editor pages, Doxygen, Mermaid, GitHub repository, official project sites.

## Mermaid Diagram Selection

- architecture: `architecture-beta` or `flowchart`
- interaction or handoff: `sequenceDiagram`
- state transitions: `stateDiagram-v2`
- release gates: `requirementDiagram`
- classification: `mindmap`, `block`, or `flowchart`
- comparison: `quadrantChart` only with explicit, measurable axes

Do not use decorative diagrams without technical value.

## Rewrite Workflow

1. Read the current file and identify narrative, mixed-language text, and unverifiable claims.
2. Reduce the document to: scope, contract, invariants, non-goals, examples.
3. Replace prose paragraphs with tables, bullet lists, and short normative statements.
4. Rewrite English first.
5. Translate/adapt Russian after English is complete.
6. Run `python3 scripts/lint-docs.py`.

## Changelog

- When a branch changes externally visible behavior, update `CHANGELOG.md` in the same branch.
- Follow Keep a Changelog 1.1.0 with factual bullets.
