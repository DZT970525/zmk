#!/usr/bin/env python3
"""Bootstrap a ZMK module repository from cormoran's custom Studio RPC template."""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
from pathlib import Path


DEFAULT_TEMPLATE_URL = (
    "git@github.com:cormoran/zmk-module-template-with-custom-studio-rpc.git"
)
DEFAULT_TEMPLATE_REF = "main+custom-studio-protocol"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create a GitHub repo and initialize it from the ZMK custom Studio RPC template.",
    )
    parser.add_argument("--repo", required=True, help="New GitHub repository name.")
    parser.add_argument(
        "--owner", default="cormoran", help="GitHub owner or organization."
    )
    parser.add_argument(
        "--visibility",
        choices=("public", "private", "internal"),
        default="private",
        help="Visibility for the new GitHub repository.",
    )
    parser.add_argument(
        "--branch",
        help="Implementation branch to create after pushing main. Defaults to codex/init-<repo>.",
    )
    parser.add_argument(
        "--destination",
        type=Path,
        help="Clone destination. Defaults to ../<repo> relative to the current directory.",
    )
    parser.add_argument(
        "--template-url",
        default=DEFAULT_TEMPLATE_URL,
        help="Template repository SSH URL.",
    )
    parser.add_argument(
        "--template-ref",
        default=DEFAULT_TEMPLATE_REF,
        help="Template ref to reset main to.",
    )
    parser.add_argument(
        "--description", default="", help="Optional GitHub repository description."
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Print commands without running them."
    )
    return parser.parse_args()


def slug_for_branch(repo: str) -> str:
    slug = repo
    if slug.startswith("zmk-"):
        slug = slug[4:]
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", slug).strip("-").lower()
    return slug or "module"


def quote_cmd(command: list[str], cwd: Path | None = None) -> str:
    rendered = " ".join(shlex.quote(part) for part in command)
    if cwd is not None:
        return f"(cd {shlex.quote(str(cwd))} && {rendered})"
    return rendered


def run(command: list[str], *, cwd: Path | None = None, dry_run: bool = False) -> None:
    print(f"+ {quote_cmd(command, cwd)}")
    if dry_run:
        return
    subprocess.run(command, cwd=cwd, check=True)


def ensure_destination_is_available(destination: Path) -> None:
    if destination.exists():
        try:
            next(destination.iterdir())
        except StopIteration:
            return
        raise SystemExit(f"Destination exists and is not empty: {destination}")


def main() -> int:
    args = parse_args()
    cwd = Path.cwd()
    destination = (args.destination or cwd.parent / args.repo).expanduser().resolve()
    origin_url = f"git@github.com:{args.owner}/{args.repo}.git"
    branch = args.branch or f"codex/init-{slug_for_branch(args.repo)}"

    ensure_destination_is_available(destination)

    repo_spec = f"{args.owner}/{args.repo}"
    create_cmd = ["gh", "repo", "create", repo_spec, f"--{args.visibility}"]
    if args.description:
        create_cmd.extend(["--description", args.description])

    print(f"Repository: {repo_spec}")
    print(f"Destination: {destination}")
    print(f"Template: {args.template_url} ({args.template_ref})")
    print(f"Implementation branch: {branch}")

    run(create_cmd, dry_run=args.dry_run)
    run(["git", "clone", args.template_url, str(destination)], dry_run=args.dry_run)
    run(
        ["git", "remote", "rename", "origin", "template"],
        cwd=destination,
        dry_run=args.dry_run,
    )
    run(
        ["git", "remote", "add", "origin", origin_url],
        cwd=destination,
        dry_run=args.dry_run,
    )
    run(
        ["git", "fetch", "template", args.template_ref],
        cwd=destination,
        dry_run=args.dry_run,
    )
    run(
        ["git", "checkout", "-B", "main", f"template/{args.template_ref}"],
        cwd=destination,
        dry_run=args.dry_run,
    )
    run(
        ["git", "push", "-u", "origin", "main", "--force"],
        cwd=destination,
        dry_run=args.dry_run,
    )
    run(["git", "switch", "-c", branch], cwd=destination, dry_run=args.dry_run)

    print()
    print("Next steps:")
    print(f"  cd {shlex.quote(str(destination))}")
    print("  Read AGENTS.md and complete the Initialization section.")
    print("  Remove only that Initialization section from AGENTS.md after conversion.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
