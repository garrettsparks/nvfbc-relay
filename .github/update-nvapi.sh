#!/bin/bash
# update-nvapi.sh

echo "Updating NvAPI submodule..."
git submodule update --remote inc/NvAPI

echo "Copying library files..."
cp ./inc/NvAPI/amd64/nvapi64.lib ./lib/NvAPI/amd64/
cp ./inc/NvAPI/x86/nvapi.lib ./lib/NvAPI/x86/

echo "Done! NvAPI submodule and libraries updated."
echo ""
echo "Changes ready to commit:"
git status --short inc/NvAPI lib/NvAPI
