#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/src/ApmConsole.Host"
dotnet run --urls "http://localhost:5299"
