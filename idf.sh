#!/usr/bin/env bash
# Запуск ESP-IDF 5.4.4, установленного через Espressif Installation Manager.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
idf_setup="$HOME/.espressif/tools/activate_idf_v5.4.4.sh"
idf_sdk="$HOME/.espressif/v5.4.4/esp-idf"
idf_tool_path=""
idf_installer_sdk=""

if [[ ! -f "$idf_setup" ]]; then
    printf '%s\n' "Не найден скрипт окружения ESP-IDF 5.4.4: $idf_setup" >&2
    exit 1
fi

# Прочитать окружение установщика без функций интерактивной оболочки.
idf_exports="$(/bin/bash "$idf_setup" -e)"
while IFS= read -r idf_pair; do
    case "$idf_pair" in
        PATH=*) idf_tool_path="${idf_pair#PATH=}" ;;
        IDF_PATH=*) idf_installer_sdk="${idf_pair#IDF_PATH=}" ;;
        IDF_TOOLS_PATH=*|IDF_PYTHON_ENV_PATH=*|ESP_ROM_ELF_DIR=*|OPENOCD_SCRIPTS=*|IDF_COMPONENT_LOCAL_STORAGE_URL=*)
            export "$idf_pair" ;;
    esac
done <<< "$idf_exports"

if [[ ! -f "$idf_sdk/tools/idf.py" ]]; then
    idf_sdk="$idf_installer_sdk"
fi
if [[ ! -f "$idf_sdk/tools/idf.py" || ! -x "${IDF_PYTHON_ENV_PATH:-}/bin/python" || -z "$idf_tool_path" ]]; then
    printf '%s\n' "Не найден SDK ESP-IDF 5.4.4 или его окружение Python." >&2
    exit 1
fi

export IDF_PATH="$idf_sdk"
export PATH="$idf_tool_path:$PATH"
cd -- "$project_dir"
if [[ $# -eq 0 ]]; then
    set -- build
fi
exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"
