#!/usr/bin/env python3
"""Thin bootstrap entry point for the user-level LingTai Desktop installer."""

from desktop_user_cli import bootstrap_main


if __name__ == "__main__":
    raise SystemExit(bootstrap_main())
