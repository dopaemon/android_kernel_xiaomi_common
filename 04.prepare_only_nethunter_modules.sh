#!/bin/bash

# ==============================================================================
#
#                    NetHunter Module Extractor Script
#
#   This script extracts NetHunter modules and their dependencies,
#   organizing them into vendor_boot and vendor_dlkm folders.
#
#   Usage:
#   ./nethunter_extractor.sh <nh_modules_dir> <staging_dir> <vendor_boot_list> <system_map> <output_dir>
#
#   Arguments:
#   <nh_modules_dir>    Directory containing suspected NetHunter modules
#   <staging_dir>       Kernel build staging directory with all modules
#   <vendor_boot_list>  Path to vendor_boot.img's modules_list.txt
#   <system_map>        Path to System.map file for depmod (optional, can be empty "")
#   <output_dir>        Output directory for organized modules
#
# ==============================================================================

set +e

# --- Functions ---

print_header() {
    echo "========================================================================"
    echo "$1"
    echo "========================================================================"
}

sanitize_path() {
    echo "$1" | sed -e "s/^'//" -e "s/'$//" -e 's/^"//' -e 's/"$//'
}

log_info() {
    echo "[INFO] $1"
}

log_warning() {
    echo "[WARNING] $1"
}

log_error() {
    echo "[ERROR] $1"
}

show_help() {
    echo "Usage: $0 [OPTIONS] [ARGUMENTS...]"
    echo ""
    echo "NetHunter Module Extractor - Organizes NetHunter modules and dependencies"
    echo ""
    echo "Modes of Operation:"
    echo "  1. Interactive Mode: Run without any arguments to be prompted for each path."
    echo "     $0"
    echo ""
    echo "  2. Non-Interactive (Argument) Mode: Provide all 5-6 paths as arguments."
    echo "     $0 <nh_modules_dir> <staging_dir> <vendor_boot_list> <system_map> <output_dir> [strip_tool]"
    echo ""
    echo "Arguments:"
    echo "  <nh_modules_dir>    Directory containing suspected NetHunter modules"
    echo "  <staging_dir>       Kernel build staging directory with all modules"
    echo "  <vendor_boot_list>  Path to vendor_boot.img's modules_list.txt (or \"\" to skip separation)"
    echo "  <system_map>        Path to System.map file for depmod (optional, use \"\" to skip)"
    echo "                      If not provided, depmod will use module metadata only"
    echo "  <output_dir>        Output directory for organized modules"
    echo "  [strip_tool]        (Optional) Path to strip tool (llvm-strip or aarch64-linux-gnu-strip)"
    echo "                      Leave empty or provide \"\" to skip stripping"
    echo ""
    echo "Options:"
    echo "  -h, --help          Show this help message and exit."
    echo ""
    echo "Output Structure:"
    echo "  output_dir/"
    echo "    ├── vendor_boot/     - Dependencies available in vendor_boot"
    echo "    └── vendor_dlkm/     - NetHunter modules + other dependencies"
    echo ""
    echo "Each folder will contain:"
    echo "  - *.ko files (stripped if tool provided)"
    echo "  - modules.dep"
    echo "  - modules.load"
    echo "  - modules.order"
    echo ""
}

find_module_in_staging() {
    local module_name="$1"
    local staging_dir="$2"
    find "$staging_dir" -name "$module_name" -type f -print -quit 2>/dev/null
}

# Function to generate proper load order based on dependencies (topological sort)
generate_load_order_from_deps() {
    local temp_deps="temp_deps.txt"
    local temp_load="temp_load.txt"
    local processed="processed_modules.txt"
    
    # Parse modules.dep to create a cleaner dependency map
    > "$temp_deps"
    > "$processed"
    
    # Extract dependencies and create a mapping
    while IFS=':' read -r module deps_line || [ -n "$module" ]; do
        [ -z "$module" ] && continue
        
        # Clean module name (remove path prefix)
        clean_module=$(basename "$module")
        
        # Clean and parse dependencies
        if [ -n "$deps_line" ]; then
            # Remove leading/trailing spaces and split dependencies
            clean_deps=$(echo "$deps_line" | sed 's/^ *//' | sed 's/ *$//')
            if [ -n "$clean_deps" ]; then
                for dep in $clean_deps; do
                    clean_dep=$(basename "$dep")
                    echo "$clean_module:$clean_dep" >> "$temp_deps"
                done
            else
                # Module has no dependencies
                echo "$clean_module:" >> "$temp_deps"
            fi
        else
            # Module has no dependencies
            echo "$clean_module:" >> "$temp_deps"
        fi
    done < modules.dep
    
    # Topological sort implementation
    > "$temp_load"
    local changed=1
    local iteration=1
    
    # Get list of all modules
    local all_modules=($(ls -1 *.ko 2>/dev/null))
    
    while [ $changed -eq 1 ] && [ $iteration -le 50 ]; do
        changed=0
        
        for module_file in "${all_modules[@]}"; do
            [ ! -f "$module_file" ] && continue
            
            # Skip if already processed
            if grep -Fxq "$module_file" "$processed" 2>/dev/null; then
                continue
            fi
            
            # Check if all dependencies are already processed
            local can_process=1
            local module_deps=""
            
            # Get dependencies for this module
            module_deps=$(grep "^$module_file:" "$temp_deps" | cut -d':' -f2- | tr ':' ' ')
            
            if [ -n "$module_deps" ]; then
                for dep in $module_deps; do
                    [ -z "$dep" ] && continue
                    if ! grep -Fxq "$dep" "$processed" 2>/dev/null; then
                        can_process=0
                        break
                    fi
                done
            fi
            
            # If all dependencies are satisfied, add this module
            if [ $can_process -eq 1 ]; then
                echo "$module_file" >> "$temp_load"
                echo "$module_file" >> "$processed"
                changed=1
            fi
        done
        
        ((iteration++))
    done
    
    # Handle any remaining modules (circular dependencies or orphans)
    for module_file in "${all_modules[@]}"; do
        [ ! -f "$module_file" ] && continue
        if ! grep -Fxq "$module_file" "$processed" 2>/dev/null; then
            echo "$module_file" >> "$temp_load"
        fi
    done
    
    # Final output
    mv "$temp_load" modules.load
    
    # Cleanup
    rm -f "$temp_deps" "$processed"
}

# Function to strip modules using strip tool
strip_modules() {
    local module_dir="$1"
    local strip_tool="$2"

    if [ -z "$strip_tool" ] || [ ! -x "$strip_tool" ]; then
        log_warning "Strip tool not available, skipping module stripping..."
        return 1
    fi

    log_info "Stripping modules in $(basename "$module_dir") to reduce size..."

    local stripped_count=0
    local failed_count=0
    local total_size_before=0
    local total_size_after=0

    # Calculate total size before stripping
    for module in "$module_dir"/*.ko; do
        [ -f "$module" ] || continue
        size_before=$(stat -f%z "$module" 2>/dev/null || stat -c%s "$module" 2>/dev/null || echo "0")
        total_size_before=$((total_size_before + size_before))
    done

    for module in "$module_dir"/*.ko; do
        [ -f "$module" ] || continue

        module_name=$(basename "$module")

        # Strip the module (remove debug info and unneeded symbols)
        "$strip_tool" --strip-debug --strip-unneeded "$module" 2>/dev/null

        if [ $? -eq 0 ]; then
            ((stripped_count++))
            log_info "  ✓ Stripped: $module_name"
        else
            ((failed_count++))
            log_warning "  ✗ Failed to strip: $module_name"
        fi
    done

    # Calculate total size after stripping
    for module in "$module_dir"/*.ko; do
        [ -f "$module" ] || continue
        size_after=$(stat -f%z "$module" 2>/dev/null || stat -c%s "$module" 2>/dev/null || echo "0")
        total_size_after=$((total_size_after + size_after))
    done

    if [ $stripped_count -gt 0 ]; then
        total_reduction=$((total_size_before - total_size_after))
        reduction_kb=$((total_reduction / 1024))
        log_info "Strip complete: $stripped_count modules stripped, $failed_count failed"
        log_info "Total size reduction: $total_reduction bytes (${reduction_kb}KB)"
    fi

    return 0
}

# --- Argument Validation ---

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    show_help
    exit 0
fi

print_header "NetHunter Module Extractor"

# --- Input Collection ---

# Non-Interactive (Argument) Mode
if [ "$#" -eq 5 ] || [ "$#" -eq 6 ]; then
    log_info "Running in Non-Interactive Mode."
    NH_MODULE_DIR_RAW="$1"
    STAGING_DIR_RAW="$2"
    VENDOR_BOOT_LIST_RAW="$3"
    SYSTEM_MAP_RAW="$4"
    OUTPUT_DIR_RAW="$5"
    STRIP_TOOL_RAW="${6:-}"  # Optional 6th argument

# Interactive Mode
elif [ "$#" -eq 0 ]; then
    log_info "Running in Interactive Mode."
    echo "This script extracts NetHunter modules and organizes them with their dependencies."
    echo "Please provide the required paths."
    echo ""
    read -e -p "Enter path to NetHunter modules directory: " NH_MODULE_DIR_RAW
    read -e -p "Enter path to kernel build staging directory: " STAGING_DIR_RAW
    read -e -p "Enter path to vendor_boot.img's modules_list.txt (or press Enter to skip separation): " VENDOR_BOOT_LIST_RAW
    read -e -p "Enter path to System.map file (or press Enter to skip - will use module metadata only): " SYSTEM_MAP_RAW
    read -e -p "Enter output directory for organized modules: " OUTPUT_DIR_RAW
    read -e -p "Enter path to strip tool (llvm-strip/aarch64-linux-gnu-strip) or press Enter to skip: " STRIP_TOOL_RAW

# Invalid arguments
else
    log_error "Invalid number of arguments. Use 0 for interactive mode or 5-6 for non-interactive."
    show_help
    exit 1
fi

# Sanitize paths
NH_MODULE_DIR=$(sanitize_path "$NH_MODULE_DIR_RAW")
STAGING_DIR=$(sanitize_path "$STAGING_DIR_RAW")
VENDOR_BOOT_LIST=$(sanitize_path "$VENDOR_BOOT_LIST_RAW")

# Handle System.map - make it optional
SKIP_SYSTEM_MAP=false
if [ -z "$SYSTEM_MAP_RAW" ]; then
    SKIP_SYSTEM_MAP=true
    SYSTEM_MAP=""
else
    SYSTEM_MAP=$(sanitize_path "$SYSTEM_MAP_RAW")
    # Check if sanitized path is also empty (user entered just quotes/spaces)
    if [ -z "$SYSTEM_MAP" ]; then
        SKIP_SYSTEM_MAP=true
        SYSTEM_MAP=""
    fi
fi

OUTPUT_DIR=$(sanitize_path "$OUTPUT_DIR_RAW")
STRIP_TOOL=$(sanitize_path "$STRIP_TOOL_RAW")

# Validate inputs
if [ ! -d "$NH_MODULE_DIR" ]; then
    log_error "NetHunter modules directory not found: '$NH_MODULE_DIR'"
    exit 1
fi

if [ ! -d "$STAGING_DIR" ]; then
    log_error "Staging directory not found: '$STAGING_DIR'"
    exit 1
fi

# Handle vendor_boot list - can be empty to skip separation
SKIP_VENDOR_BOOT_SEPARATION=false
if [ -z "$VENDOR_BOOT_LIST" ]; then
    SKIP_VENDOR_BOOT_SEPARATION=true
    log_info "Vendor boot modules list not provided - will place all modules in vendor_dlkm only"
elif [ ! -f "$VENDOR_BOOT_LIST" ]; then
    log_error "Vendor boot modules list not found: '$VENDOR_BOOT_LIST'"
    exit 1
fi

# Validate System.map if provided
if [ "$SKIP_SYSTEM_MAP" = false ] && [ -n "$SYSTEM_MAP" ]; then
    if [ ! -f "$SYSTEM_MAP" ]; then
        log_error "System.map file not found: '$SYSTEM_MAP'"
        exit 1
    fi
    log_info "System.map provided: $SYSTEM_MAP"
else
    log_info "System.map not provided - depmod will use module metadata only"
fi

# Handle optional output directory
if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR="$(pwd)/nethunter_modules_extracted"
    log_info "Output directory not specified, using: $OUTPUT_DIR"
fi

# Validate strip tool if provided
if [ -n "$STRIP_TOOL" ] && [ ! -x "$STRIP_TOOL" ]; then
    log_warning "Strip tool not found or not executable: '$STRIP_TOOL'. Module stripping will be skipped."
    STRIP_TOOL=""
fi

print_header "Initialization"

print_header "Setup and Validation"

# --- Input Sanitization and Validation ---

# Create clean output directory structure
log_info "Creating output directory structure..."
rm -rf "$OUTPUT_DIR"
if [ "$SKIP_VENDOR_BOOT_SEPARATION" = false ]; then
    mkdir -p "$OUTPUT_DIR/vendor_boot"
    log_info "Created vendor_boot directory"
fi
mkdir -p "$OUTPUT_DIR/vendor_dlkm"
log_info "Created vendor_dlkm directory"

# Initialize missing modules tracking file
MISSING_MODULES_FILE="$OUTPUT_DIR/missing_modules.txt"
MISSING_MODULES=()
> "$MISSING_MODULES_FILE"

# Create temporary work directories
WORK_DIR=$(mktemp -d)
ALL_DEPS_DIR="$WORK_DIR/all_deps"
mkdir -p "$ALL_DEPS_DIR"

trap 'log_info "Cleaning up temporary directories..."; rm -rf "$WORK_DIR"' EXIT

# Detect kernel version
FIRST_KO=$(find "$STAGING_DIR" -name "*.ko" -print -quit)
if [ -z "$FIRST_KO" ]; then
    log_error "No .ko files found in staging directory"
    exit 1
fi

KERNEL_VERSION=$(modinfo "$FIRST_KO" | grep -m 1 "vermagic:" | awk '{print $2}')
if [ -z "$KERNEL_VERSION" ]; then
    log_error "Could not determine kernel version from modules"
    exit 1
fi

log_info "Detected kernel version: $KERNEL_VERSION"

# Setup module directory structures
if [ "$SKIP_VENDOR_BOOT_SEPARATION" = false ]; then
    VB_MODULE_DIR="$OUTPUT_DIR/vendor_boot/lib/modules/$KERNEL_VERSION"
    mkdir -p "$VB_MODULE_DIR"
fi
VD_MODULE_DIR="$OUTPUT_DIR/vendor_dlkm/lib/modules/$KERNEL_VERSION"
mkdir -p "$VD_MODULE_DIR"

# --- NetHunter Module Collection ---

print_header "Collecting NetHunter Modules"

# Copy all NetHunter modules to working directory
NH_MODULES=()
NH_COUNT=0

for module_path in "$NH_MODULE_DIR"/*.ko; do
    [ -f "$module_path" ] || continue
    module_name=$(basename "$module_path")
    cp "$module_path" "$ALL_DEPS_DIR/"
    NH_MODULES+=("$module_name")
    ((NH_COUNT++))
    log_info "✓ Found NetHunter module: $module_name"
done

log_info "Collected $NH_COUNT NetHunter modules"

if [ $NH_COUNT -eq 0 ]; then
    log_error "No NetHunter modules found in directory: $NH_MODULE_DIR"
    exit 1
fi

# --- Dependency Resolution ---

print_header "Resolving Dependencies"

# Create a list to track all modules we need (NetHunter + dependencies)
ALL_REQUIRED_MODULES=("${NH_MODULES[@]}")

# Iteratively resolve dependencies
NEW_DEPS_FOUND=1
ITERATION=1

while [ "$NEW_DEPS_FOUND" -gt 0 ]; do
    NEW_DEPS_FOUND=0
    log_info "Dependency resolution iteration $ITERATION..."
    
    # Check dependencies for all modules currently in our collection
    for module_path in "$ALL_DEPS_DIR"/*.ko; do
        [ -f "$module_path" ] || continue
        
        # Get dependencies for this module
        deps=$(modinfo -F depends "$module_path" 2>/dev/null | tr ',' '\n' | grep -v '^$')
        
        for dep_name in $deps; do
            dep_ko_name="${dep_name}.ko"
            
            # Check if we already have this dependency
            if [ ! -f "$ALL_DEPS_DIR/$dep_ko_name" ]; then
                # Find dependency in staging
                dep_path=$(find_module_in_staging "$dep_ko_name" "$STAGING_DIR")
                
                if [ -n "$dep_path" ] && [ -f "$dep_path" ]; then
                    log_info "  ✓ Adding dependency: $dep_ko_name"
                    cp "$dep_path" "$ALL_DEPS_DIR/"
                    ALL_REQUIRED_MODULES+=("$dep_ko_name")
                    ((NEW_DEPS_FOUND++))
                else
                    log_warning "  ✗ Dependency not found: $dep_ko_name"
                    # Track missing dependency if not already tracked
                    if ! grep -Fxq "$dep_ko_name" "$MISSING_MODULES_FILE" 2>/dev/null; then
                        echo "$dep_ko_name" >> "$MISSING_MODULES_FILE"
                        MISSING_MODULES+=("$dep_ko_name")
                    fi
                fi
            fi
        done
    done
    
    ((ITERATION++))
    [ "$NEW_DEPS_FOUND" -eq 0 ] && log_info "Dependency resolution complete after $((ITERATION-1)) iterations"
done

TOTAL_MODULES=${#ALL_REQUIRED_MODULES[@]}
log_info "Total modules required: $TOTAL_MODULES (NetHunter + dependencies)"

# --- Module Organization ---

print_header "Organizing Modules into Output Directories"

if [ "$SKIP_VENDOR_BOOT_SEPARATION" = true ]; then
    log_info "Vendor boot separation skipped - placing all modules in vendor_dlkm"
    
    # Copy all modules to vendor_dlkm
    for module_name in "${ALL_REQUIRED_MODULES[@]}"; do
        module_path="$ALL_DEPS_DIR/$module_name"
        [ -f "$module_path" ] || continue
        
        cp "$module_path" "$VD_MODULE_DIR/"
        ((VD_COUNT++))
        log_info "→ vendor_dlkm: $module_name"
    done
    
    VB_COUNT=0
    log_info "All $VD_COUNT modules placed in vendor_dlkm"
    
else
    log_info "Organizing modules based on vendor_boot modules list"
    
    # Read vendor_boot modules list into array for faster lookup
    declare -A VENDOR_BOOT_MODULES
    while IFS= read -r module_name || [ -n "$module_name" ]; do
        [ -z "$module_name" ] && continue
        module_name=$(echo "$module_name" | tr -d '\r\n' | xargs)
        [ -z "$module_name" ] && continue
        VENDOR_BOOT_MODULES["$module_name"]=1
    done < "$VENDOR_BOOT_LIST"

    VB_COUNT=0
    VD_COUNT=0

    # Organize modules
    for module_name in "${ALL_REQUIRED_MODULES[@]}"; do
        module_path="$ALL_DEPS_DIR/$module_name"
        [ -f "$module_path" ] || continue
        
        if [[ -n "${VENDOR_BOOT_MODULES[$module_name]}" ]]; then
            # Module should go to vendor_boot
            cp "$module_path" "$VB_MODULE_DIR/"
            ((VB_COUNT++))
            log_info "→ vendor_boot: $module_name"
        else
            # Module should go to vendor_dlkm
            cp "$module_path" "$VD_MODULE_DIR/"
            ((VD_COUNT++))
            log_info "→ vendor_dlkm: $module_name"
        fi
    done

    log_info "Organized $VB_COUNT modules into vendor_boot"
    log_info "Organized $VD_COUNT modules into vendor_dlkm"
fi

# --- Module Stripping ---

if [ -n "$STRIP_TOOL" ]; then
    print_header "Stripping Modules"
    
    # Strip vendor_boot modules if any and not skipped
    if [ "$SKIP_VENDOR_BOOT_SEPARATION" = false ] && [ $VB_COUNT -gt 0 ]; then
        strip_modules "$VB_MODULE_DIR" "$STRIP_TOOL"
    fi
    
    # Strip vendor_dlkm modules if any
    if [ $VD_COUNT -gt 0 ]; then
        strip_modules "$VD_MODULE_DIR" "$STRIP_TOOL"
    fi
else
    log_info "Strip tool not provided, skipping module stripping..."
fi

# --- Copy Build Files ---

print_header "Preparing Build Environment"

# Find and copy required build files from staging
STAGING_MODULES_DIR=$(dirname "$(find "$STAGING_DIR" -type f -name "modules.builtin" -path "*/lib/modules/*" | head -1)")

if [ -d "$STAGING_MODULES_DIR" ]; then
    log_info "Copying build files from staging..."
    
    # Copy to vendor_boot if it exists and has modules
    if [ "$SKIP_VENDOR_BOOT_SEPARATION" = false ] && [ $VB_COUNT -gt 0 ]; then
        cp "$STAGING_MODULES_DIR"/modules.* "$VB_MODULE_DIR/" 2>/dev/null
        log_info "✓ Build files copied to vendor_boot"
    fi
    
    # Copy to vendor_dlkm if it has modules
    if [ $VD_COUNT -gt 0 ]; then
        cp "$STAGING_MODULES_DIR"/modules.* "$VD_MODULE_DIR/" 2>/dev/null
        log_info "✓ Build files copied to vendor_dlkm"
    fi
else
    log_warning "Could not find staging modules directory for build files"
fi

# --- Generate Module Dependencies and Load Orders ---

print_header "Generating Module Dependencies and Load Orders"

# Function to generate dependencies and load order for a directory
generate_module_files() {
    local base_dir="$1"
    local dir_name="$2"
    
    if [ ! -d "$base_dir/lib/modules/$KERNEL_VERSION" ]; then
        return
    fi
    
    local module_dir="$base_dir/lib/modules/$KERNEL_VERSION"
    local module_count=$(ls -1 "$module_dir"/*.ko 2>/dev/null | wc -l)
    
    if [ $module_count -eq 0 ]; then
        log_warning "No modules found in $dir_name, skipping..."
        return
    fi
    
    log_info "Processing $dir_name ($module_count modules)..."
    
    # Run depmod to generate modules.dep
    cd "$base_dir"
    if [ "$SKIP_SYSTEM_MAP" = false ] && [ -n "$SYSTEM_MAP" ] && [ -f "$SYSTEM_MAP" ]; then
        # Use System.map if provided
        depmod -b . -F "$SYSTEM_MAP" "$KERNEL_VERSION"
    else
        # Use module metadata only (no System.map)
        depmod -b . "$KERNEL_VERSION"
    fi
    
    if [ $? -eq 0 ]; then
        log_info "✓ Generated modules.dep for $dir_name"
    else
        log_error "✗ Failed to generate modules.dep for $dir_name"
        return
    fi
    
    cd "$module_dir"
    
    # Generate modules.load with proper dependency order (topological sort)
    generate_load_order_from_deps
    log_info "✓ Generated modules.load for $dir_name"
    
    # Generate modules.order (same as modules.load for simplicity)
    cp modules.load modules.order
    log_info "✓ Generated modules.order for $dir_name"
    
    log_info "✓ Module files generated for $dir_name"
}

# Generate files for vendor_boot
if [ "$SKIP_VENDOR_BOOT_SEPARATION" = false ] && [ $VB_COUNT -gt 0 ]; then
    generate_module_files "$OUTPUT_DIR/vendor_boot" "vendor_boot"
fi

# Generate files for vendor_dlkm
if [ $VD_COUNT -gt 0 ]; then
    generate_module_files "$OUTPUT_DIR/vendor_dlkm" "vendor_dlkm"
fi

# --- Final Summary ---

print_header "Extraction Complete!"

echo "NetHunter module extraction successful!"
echo ""
echo "Summary:"
echo "  - NetHunter modules found: $NH_COUNT"
echo "  - Total modules processed: $TOTAL_MODULES"
echo "  - vendor_boot modules: $VB_COUNT"
echo "  - vendor_dlkm modules: $VD_COUNT"
echo "  - Vendor boot separation: $([ "$SKIP_VENDOR_BOOT_SEPARATION" = true ] && echo "Skipped" || echo "Enabled")"
echo "  - System.map used: $([ "$SKIP_SYSTEM_MAP" = false ] && [ -n "$SYSTEM_MAP" ] && echo "$(basename "$SYSTEM_MAP")" || echo "None (using module metadata)")"
echo "  - Strip tool used: $([ -n "$STRIP_TOOL" ] && echo "$(basename "$STRIP_TOOL")" || echo "None")"
echo "  - Output directory: $OUTPUT_DIR"
echo ""

# Check and display missing modules warning
if [ -f "$MISSING_MODULES_FILE" ] && [ -s "$MISSING_MODULES_FILE" ]; then
    MISSING_COUNT=$(wc -l < "$MISSING_MODULES_FILE" | tr -d ' ')
    print_header "⚠️  MISSING MODULES WARNING"
    echo "The following $MISSING_COUNT module(s) were NOT found in the staging directory:"
    echo ""
    cat "$MISSING_MODULES_FILE" | sed 's/^/  - /'
    echo ""
    echo "These modules may be:"
    echo "  1. Not compiled (enable them in kernel config and rebuild)"
    echo "  2. Proprietary modules (extract from stock ROM)"
    echo "  3. Out-of-tree modules (compile separately)"
    echo ""
    echo "Missing modules list saved to: $MISSING_MODULES_FILE"
    echo ""
    log_warning "Some modules are missing - review the list above and take appropriate action"
    echo ""
fi

if [ "$SKIP_VENDOR_BOOT_SEPARATION" = false ] && [ $VB_COUNT -gt 0 ]; then
    echo "vendor_boot contents:"
    ls -la "$OUTPUT_DIR/vendor_boot/lib/modules/$KERNEL_VERSION/" 2>/dev/null || echo "  (no files)"
    echo ""
fi

if [ $VD_COUNT -gt 0 ]; then
    echo "vendor_dlkm contents:"
    ls -la "$OUTPUT_DIR/vendor_dlkm/lib/modules/$KERNEL_VERSION/" 2>/dev/null || echo "  (no files)"
    echo ""
fi

echo "Your NetHunter modules are ready for integration!"
