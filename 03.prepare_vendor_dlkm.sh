#!/bin/bash

# ==============================================================================
#
#                    Vendor DLKM Modules Preparation Script
#
#   This script prepares vendor_dlkm modules with intelligent dependency
#   resolution and load order optimization for Android GKI kernels.
#
#   It supports two modes:
#   1. Interactive: Prompts the user for all required paths.
#   2. Non-Interactive: Accepts all paths as command-line arguments.
#
#   Enhanced workflow for NetHunter:
#   1.  Copy all the modules listed in the modules_list.txt of the vendor_dlkm.img
#   2.  Copy all the suspected nethunter modules to a "different" folder "temporary".
#   3.  Copy missing dependencies for all the nethunter kernel modules of that temp folder
#       from the staging directory to the temp folder.
#   4.  Prune the modules, which are already defined in vendor_boot's module_list.txt,
#       from that nethunter temp folder.
#   5.  Copy the final .ko modules, located inside that temp folder to the main module folder
#   6.  Strip all modules to reduce size.
#   7.  Run depmod to create the modules.dep from the modules listed in the folder.
#   8.  Append / insert, added / new modules to the new modules.load file,
#       generating from the OEM's one.
#
#                              - ravindu644
# ==============================================================================

# Disable exit on error temporarily for better control
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

# Function to display inline progress (overwrites same line)
show_progress() {
    local current=$1
    local total=$2
    local action=$3
    local name=$4
    local percent=$((current * 100 / total))
    printf "\r[INFO] %s: %d/%d (%d%%) %s" "$action" "$current" "$total" "$percent" "${name:0:50}"
    [ -z "$name" ] || printf "%-50s" ""  # Clear remaining space
}

# Function to finish progress line (move to next line)
finish_progress() {
    printf "\n"
}

show_help() {
    echo "Usage: $0 [OPTIONS] [ARGUMENTS...]"
    echo ""
    echo "This script prepares a vendor_dlkm module set with dependency resolution and load order optimization."
    echo ""
    echo "Modes of Operation:"
    echo "  1. Interactive Mode: Run without any arguments to be prompted for each path."
    echo "     $0"
    echo ""
    echo "  2. Non-Interactive (Argument) Mode: Provide all 8-9 paths as arguments."
    echo "     $0 <modules_list> <staging_dir> <oem_load_file> <system_map> <strip_tool> <output_dir> <vendor_boot_list> <nh_dir> [blacklist_file]"
    echo ""
    echo "Arguments:"
    echo "  <modules_list>      Path to vendor_dlkm.img's modules_list.txt"
    echo "  <staging_dir>       Path to kernel build staging directory"
    echo "  <oem_load_file>     Path to OEM vendor_dlkm.modules.load file"
    echo "  <system_map>        Path to System.map file"
    echo "  <strip_tool>        Path to LLVM strip tool (e.g., .../bin/llvm-strip)"
    echo "  <output_dir>        Output directory for the final modules"
    echo "  <vendor_boot_list>  (Optional) Path to vendor_boot.img's module_list.txt for pruning."
    echo "                      Provide an empty string \"\" to skip."
    echo "  <nh_dir>            (Optional) Path to NetHunter modules directory."
    echo "                      Provide an empty string \"\" to skip."
    echo "  [blacklist_file]    (Optional) Path to modules.blacklist file containing module names"
    echo "                      to exclude from final output (one module per line, e.g., sec.ko)"
    echo "                      Blacklisted modules will be pruned after dependency resolution."
    echo ""
    echo "Options:"
    echo "  -h, --help          Show this help message and exit."
    echo ""
}

# Function to check if a file exists in staging using find
find_module_in_staging() {
    local module_name="$1"
    local staging_dir="$2"
    find "$staging_dir" -name "$module_name" -type f -print -quit 2>/dev/null
}

# Function to strip modules using LLVM strip
strip_modules() {
    local module_dir="$1"
    local strip_tool="$2"

    if [ ! -x "$strip_tool" ]; then
        log_warning "LLVM strip tool not found or not executable: $strip_tool"
        log_warning "Skipping module stripping..."
        return 1
    fi

    local stripped_count=0
    local total_size_before=0
    local total_size_after=0
    local module_count=0
    local current_index=0

    # Count modules first
    for module in "$module_dir"/*.ko; do
        [ -f "$module" ] || continue
        ((module_count++))
        size_before=$(stat -f%z "$module" 2>/dev/null || stat -c%s "$module" 2>/dev/null || echo "0")
        total_size_before=$((total_size_before + size_before))
    done

    for module in "$module_dir"/*.ko; do
        [ -f "$module" ] || continue
        ((current_index++))
        module_name=$(basename "$module")

        show_progress "$current_index" "$module_count" "Stripping modules" "$module_name"
        "$strip_tool" --strip-debug --strip-unneeded "$module" 2>/dev/null

        if [ $? -eq 0 ]; then
            ((stripped_count++))
        fi
    done
    finish_progress

    # Calculate total size after stripping
    for module in "$module_dir"/*.ko; do
        [ -f "$module" ] || continue
        size_after=$(stat -f%z "$module" 2>/dev/null || stat -c%s "$module" 2>/dev/null || echo "0")
        total_size_after=$((total_size_after + size_after))
    done

    if [ $stripped_count -gt 0 ]; then
        total_reduction=$((total_size_before - total_size_after))
        reduction_kb=$((total_reduction / 1024))
        log_info "Stripped $stripped_count/$module_count modules - Reduced by ${reduction_kb}KB"
    fi

    return 0
}


# --- Main Script ---

# Argument Parsing
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    show_help
    exit 0
fi

print_header "Vendor DLKM Modules Preparation Script"

# --- Input Collection ---

# Non-Interactive (Argument) Mode
if [ "$#" -eq 8 ] || [ "$#" -eq 9 ]; then
    log_info "Running in Non-Interactive Mode."
    MODULES_LIST_RAW="$1"
    STAGING_DIR_RAW="$2"
    OEM_LOAD_FILE_RAW="$3"
    SYSTEM_MAP_RAW="$4"
    STRIP_TOOL_RAW="$5"
    OUTPUT_DIR_RAW="$6"
    VENDOR_BOOT_MODULES_LIST_RAW="$7" # Can be ""
    NH_MODULE_DIR_RAW="$8"             # Can be ""
    BLACKLIST_FILE_RAW="${9:-}"        # Optional 9th argument

# Interactive Mode
elif [ "$#" -eq 0 ]; then
    log_info "Running in Interactive Mode."
    echo "This script prepares a complete vendor_dlkm module set."
    echo "Please provide the required paths."
    echo ""
    read -e -p "Enter path to vendor_dlkm.img's modules_list.txt: " MODULES_LIST_RAW
    read -e -p "Enter path to kernel build staging directory: " STAGING_DIR_RAW
    read -e -p "Enter path to OEM vendor_dlkm.modules.load file: " OEM_LOAD_FILE_RAW
    read -e -p "Enter path to System.map file: " SYSTEM_MAP_RAW
    read -e -p "Enter path to LLVM strip tool (e.g., clang-rXXXXXX/bin/llvm-strip): " STRIP_TOOL_RAW
    read -e -p "Enter output directory for vendor_dlkm modules: " OUTPUT_DIR_RAW
    read -e -p "Enter path to vendor_boot.img's module_list.txt (press Enter to skip): " VENDOR_BOOT_MODULES_LIST_RAW
    read -e -p "Enter path to NetHunter modules directory (press Enter to skip): " NH_MODULE_DIR_RAW
    read -e -p "Enter path to modules.blacklist file (press Enter to skip): " BLACKLIST_FILE_RAW

# Invalid arguments
else
    log_error "Invalid number of arguments. Use 0 for interactive mode or 8-9 for non-interactive."
    show_help
    exit 1
fi


print_header "Setup and Validation"

# --- Input Sanitize and Validation ---
MODULES_LIST=$(sanitize_path "$MODULES_LIST_RAW")
STAGING_DIR=$(sanitize_path "$STAGING_DIR_RAW")
OEM_LOAD_FILE=$(sanitize_path "$OEM_LOAD_FILE_RAW")
SYSTEM_MAP=$(sanitize_path "$SYSTEM_MAP_RAW")
STRIP_TOOL=$(sanitize_path "$STRIP_TOOL_RAW")
OUTPUT_DIR=$(sanitize_path "$OUTPUT_DIR_RAW")
VENDOR_BOOT_MODULES_LIST=$(sanitize_path "$VENDOR_BOOT_MODULES_LIST_RAW")
NH_MODULE_DIR=$(sanitize_path "$NH_MODULE_DIR_RAW")
BLACKLIST_FILE=$(sanitize_path "$BLACKLIST_FILE_RAW")

# Validate mandatory inputs
if [ ! -f "$MODULES_LIST" ]; then
    log_error "vendor_dlkm modules_list.txt not found at: '$MODULES_LIST'"
    exit 1
fi
if [ ! -d "$STAGING_DIR" ]; then
    log_error "Staging directory not found: '$STAGING_DIR'"
    exit 1
fi
if [ ! -f "$OEM_LOAD_FILE" ]; then
    log_error "OEM modules.load file not found: '$OEM_LOAD_FILE'"
    exit 1
fi
if [ ! -f "$SYSTEM_MAP" ]; then
    log_error "System.map file not found: '$SYSTEM_MAP'"
    exit 1
fi

# Handle optional output directory
if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR="$(pwd)/vendor_dlkm_modules"
fi

# Validate optional inputs and give warnings
if [ -n "$VENDOR_BOOT_MODULES_LIST" ] && [ ! -f "$VENDOR_BOOT_MODULES_LIST" ]; then
    log_warning "vendor_boot module_list.txt not found: '$VENDOR_BOOT_MODULES_LIST'. Pruning will be skipped."
    VENDOR_BOOT_MODULES_LIST="" # Unset to prevent errors
fi
if [ -n "$NH_MODULE_DIR" ] && [ ! -d "$NH_MODULE_DIR" ]; then
    log_warning "NetHunter module directory not found: '$NH_MODULE_DIR'. NetHunter processing will be skipped."
    NH_MODULE_DIR="" # Unset to prevent errors
fi
if [ -n "$STRIP_TOOL" ] && [ ! -x "$STRIP_TOOL" ]; then
    log_warning "LLVM strip tool not found or not executable: '$STRIP_TOOL'. Module stripping will be skipped."
    STRIP_TOOL="" # Unset to prevent errors
fi

# Handle blacklist file - optional
SKIP_BLACKLIST=false
if [ -z "$BLACKLIST_FILE" ]; then
    SKIP_BLACKLIST=true
elif [ ! -f "$BLACKLIST_FILE" ]; then
    log_warning "Blacklist file not found: '$BLACKLIST_FILE'. Blacklist pruning will be skipped."
    SKIP_BLACKLIST=true
    BLACKLIST_FILE=""
else
    log_info "Blacklist file provided: $BLACKLIST_FILE"
fi

# Create clean output directory
log_info "Creating clean output directory: $OUTPUT_DIR"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"
MISSING_MODULES_FILE="$OUTPUT_DIR/missing_modules.txt"
> "$MISSING_MODULES_FILE"  # Initialize missing modules file

# Create temporary work directory
WORK_DIR=$(mktemp -d)
NH_TEMP_DIR=$(mktemp -d)
trap 'log_info "Cleaning up temporary directories..."; rm -rf "$WORK_DIR" "$NH_TEMP_DIR"' EXIT

# Detect kernel version from staging directory
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

# Setup module work directory structure
MODULE_WORK_DIR="$WORK_DIR/lib/modules/$KERNEL_VERSION"
mkdir -p "$MODULE_WORK_DIR"

# --- Initial Module Copy (from vendor_dlkm list) ---

print_header "Copying Initial Modules from vendor_dlkm List"

# Count total modules first
TOTAL_MODULES_TO_COPY=$(wc -l < "$MODULES_LIST" | tr -d ' ')
INITIAL_COUNT=0
MISSING_MODULES=()
CURRENT_INDEX=0

while IFS= read -r module_name || [ -n "$module_name" ]; do
    [ -z "$module_name" ] && continue
    module_name=$(echo "$module_name" | tr -d '\r\n' | xargs)
    [ -z "$module_name" ] && continue
    
    ((CURRENT_INDEX++))
    module_path=$(find_module_in_staging "$module_name" "$STAGING_DIR")

    if [ -n "$module_path" ] && [ -f "$module_path" ]; then
        cp "$module_path" "$MODULE_WORK_DIR/" 2>/dev/null
        if [ $? -eq 0 ]; then
            ((INITIAL_COUNT++))
            show_progress "$CURRENT_INDEX" "$TOTAL_MODULES_TO_COPY" "Copying modules" "$module_name"
        else
            MISSING_MODULES+=("$module_name")
            echo "$module_name" >> "$MISSING_MODULES_FILE"
            show_progress "$CURRENT_INDEX" "$TOTAL_MODULES_TO_COPY" "Copying modules" "$module_name (FAILED)"
        fi
    else
        MISSING_MODULES+=("$module_name")
        echo "$module_name" >> "$MISSING_MODULES_FILE"
        show_progress "$CURRENT_INDEX" "$TOTAL_MODULES_TO_COPY" "Copying modules" "$module_name (NOT FOUND)"
    fi
done < "$MODULES_LIST"
finish_progress

log_info "Copied $INITIAL_COUNT/$TOTAL_MODULES_TO_COPY modules"
if [ ${#MISSING_MODULES[@]} -gt 0 ]; then
    log_warning "Missing ${#MISSING_MODULES[@]} modules from staging directory"
fi

# --- NetHunter Module Processing ---

NH_COUNT=0
PRUNED_COUNT=0
if [ -n "$NH_MODULE_DIR" ]; then
    print_header "Processing NetHunter Modules"

    # 1. Copy suspected NetHunter modules to a temporary, isolated directory
    find "$NH_MODULE_DIR" -name "*.ko" -exec cp {} "$NH_TEMP_DIR/" \; 2>/dev/null
    INITIAL_NH_COUNT=$(find "$NH_TEMP_DIR" -name "*.ko" | wc -l)
    log_info "Found $INITIAL_NH_COUNT NetHunter modules"

    # 2. Resolve dependencies for NetHunter modules in their isolated directory
    PROCESSED_NH_MODULES="$WORK_DIR/processed_nh.list"
    find "$NH_TEMP_DIR" -name "*.ko" -printf "%f\n" > "$PROCESSED_NH_MODULES"

    NEW_DEPS_FOUND=1
    ITERATION=0
    while [ "$NEW_DEPS_FOUND" -gt 0 ]; do
        ((ITERATION++))
        NEW_DEPS_FOUND=0

        # Create a list of all modules currently in the temp dir
        CURRENT_MODULES_IN_TEMP=$(find "$NH_TEMP_DIR" -name "*.ko" -printf "%f\n")
        TEMP_MODULE_COUNT=$(echo "$CURRENT_MODULES_IN_TEMP" | wc -l | tr -d ' ')
        PROCESSED_COUNT=0

        for module_path in "$NH_TEMP_DIR"/*.ko; do
            [ -f "$module_path" ] || continue
            ((PROCESSED_COUNT++))
            module_name=$(basename "$module_path")
            show_progress "$PROCESSED_COUNT" "$TEMP_MODULE_COUNT" "Resolving NH dependencies (iter $ITERATION)" "$module_name"
            
            deps=$(modinfo -F depends "$module_path" 2>/dev/null | tr ',' '\n' | grep -v '^$')
            for dep_name in $deps; do
                dep_ko_name="${dep_name}.ko"

                # Check if dependency is already present in the temp dir
                if ! echo "$CURRENT_MODULES_IN_TEMP" | grep -Fxq "$dep_ko_name"; then
                    dep_path=$(find_module_in_staging "$dep_ko_name" "$STAGING_DIR")
                    if [ -n "$dep_path" ]; then
                        cp "$dep_path" "$NH_TEMP_DIR/" 2>/dev/null
                        ((NEW_DEPS_FOUND++))
                    else
                        # Track missing dependency if not already tracked
                        if ! grep -Fxq "$dep_ko_name" "$MISSING_MODULES_FILE" 2>/dev/null; then
                            echo "$dep_ko_name" >> "$MISSING_MODULES_FILE"
                            MISSING_MODULES+=("$dep_ko_name")
                        fi
                    fi
                fi
            done
        done
        finish_progress
        [ "$NEW_DEPS_FOUND" -eq 0 ] && log_info "NetHunter dependency resolution complete"
    done

    # 3. Prune modules from the NetHunter temp folder (if list is provided)
    if [ -n "$VENDOR_BOOT_MODULES_LIST" ]; then
        while IFS= read -r module_to_prune || [ -n "$module_to_prune" ]; do
            [ -z "$module_to_prune" ] && continue
            module_to_prune=$(echo "$module_to_prune" | tr -d '\r\n' | xargs)

            if [ -f "$NH_TEMP_DIR/$module_to_prune" ]; then
                rm -f "$NH_TEMP_DIR/$module_to_prune"
                ((PRUNED_COUNT++))
            fi
        done < "$VENDOR_BOOT_MODULES_LIST"
        [ $PRUNED_COUNT -gt 0 ] && log_info "Pruned $PRUNED_COUNT modules (present in vendor_boot)"
    fi

    # 4. Copy final NetHunter modules to the main module folder
    cp "$NH_TEMP_DIR"/*.ko "$MODULE_WORK_DIR/" 2>/dev/null
    NH_COUNT=$(find "$NH_TEMP_DIR" -name "*.ko" 2>/dev/null | wc -l)
    [ $NH_COUNT -gt 0 ] && log_info "Merged $NH_COUNT NetHunter modules"
else
    log_info "NetHunter module directory not provided, skipping NetHunter processing."
fi

# --- Prune Blacklisted Modules ---
BLACKLISTED_COUNT=0
if [ "$SKIP_BLACKLIST" = false ]; then
    print_header "Pruning Blacklisted Modules"
    
    # Read blacklist into array
    declare -A BLACKLISTED_MODULES
    while IFS= read -r module_name || [ -n "$module_name" ]; do
        [ -z "$module_name" ] && continue
        module_name=$(echo "$module_name" | tr -d '\r\n' | xargs)
        [ -z "$module_name" ] && continue
        # Ensure .ko extension
        [[ "$module_name" == *.ko ]] || module_name="${module_name}.ko"
        BLACKLISTED_MODULES["$module_name"]=1
    done < "$BLACKLIST_FILE"
    
    # Prune blacklisted modules from working directory
    for module_name in "${!BLACKLISTED_MODULES[@]}"; do
        if [ -f "$MODULE_WORK_DIR/$module_name" ]; then
            rm -f "$MODULE_WORK_DIR/$module_name"
            ((BLACKLISTED_COUNT++))
            log_info "  ✗ Pruned blacklisted module: $module_name"
        fi
    done
    
    if [ $BLACKLISTED_COUNT -gt 0 ]; then
        log_info "Pruned $BLACKLISTED_COUNT blacklisted module(s)"
        
        # Re-resolve dependencies after pruning (to clean up orphaned dependencies)
        print_header "Re-resolving Dependencies After Blacklist Pruning"
        
        # Find modules that depend on blacklisted modules and remove them if they become orphaned
        ITERATION=0
        CHANGED=1
        while [ $CHANGED -eq 1 ] && [ $ITERATION -lt 5 ]; do
            CHANGED=0
            ((ITERATION++))
            
            for module_path in "$MODULE_WORK_DIR"/*.ko; do
                [ -f "$module_path" ] || continue
                module_name=$(basename "$module_path")
                
                # Get dependencies for this module
                deps=$(modinfo -F depends "$module_path" 2>/dev/null | tr ',' '\n' | grep -v '^$')
                
                if [ -n "$deps" ]; then
                    for dep_name in $deps; do
                        dep_ko_name="${dep_name}.ko"
                        # If dependency is blacklisted or missing, check if module should be removed
                        if [[ -n "${BLACKLISTED_MODULES[$dep_ko_name]}" ]] || [ ! -f "$MODULE_WORK_DIR/$dep_ko_name" ]; then
                            # Check if this dependency is critical (module won't load without it)
                            # For now, we'll be conservative and only remove if explicitly blacklisted
                            if [[ -n "${BLACKLISTED_MODULES[$dep_ko_name]}" ]]; then
                                log_warning "  ⚠ Module $module_name depends on blacklisted $dep_ko_name"
                                # Optionally remove the dependent module too (uncomment if needed)
                                # rm -f "$module_path"
                                # ((CHANGED++))
                            fi
                        fi
                    done
                fi
            done
        done
    else
        log_info "No blacklisted modules found in current module set"
    fi
else
    log_info "Blacklist file not provided, skipping blacklist pruning"
fi

# --- Final Dependency Resolution for all modules ---
print_header "Resolving All Final Dependencies"

# --- Strip ALL Modules ---

print_header "Stripping All Modules"
if [ -n "$STRIP_TOOL" ]; then
    strip_modules "$MODULE_WORK_DIR" "$STRIP_TOOL"
else
    log_warning "Skipping module stripping - LLVM strip tool not available"
fi

# --- Copy Required Build Files ---

print_header "Preparing Build Environment"
STAGING_MODULES_DIR=$(dirname "$(find "$STAGING_DIR" -type f -name "modules.builtin" -path "*/lib/modules/*" | head -1)")
if [ -d "$STAGING_MODULES_DIR" ]; then
    cp "$STAGING_MODULES_DIR"/modules.* "$MODULE_WORK_DIR/" 2>/dev/null
    log_info "Build files copied"
fi

# --- Generate New modules.dep ---

print_header "Generating Module Dependencies"
cd "$WORK_DIR"
depmod -b . -F "$SYSTEM_MAP" "$KERNEL_VERSION" 2>/dev/null
if [ $? -ne 0 ]; then
    log_error "depmod failed."
    exit 1
fi
log_info "Module dependencies generated"

# --- Intelligent modules.load Generation ---

print_header "Generating Optimized modules.load"
cd "$MODULE_WORK_DIR"

# Create list of our current modules
find . -maxdepth 1 -name "*.ko" -printf "%f\n" > our_modules.list

# Clean up OEM modules.load
sed 's/\r$//' "$OEM_LOAD_FILE" > oem_modules.list

# Create base load order from intersection
grep -Fx -f our_modules.list oem_modules.list > modules.load.base

# Find new modules that need to be inserted
grep -Fxv -f oem_modules.list our_modules.list > new_modules.list

cp modules.load.base modules.load.final
NEW_MODULE_COUNT=$(wc -l < new_modules.list)

if [ "$NEW_MODULE_COUNT" -gt 0 ]; then
    CURRENT_INDEX=0
    > insertions.tsv
    while IFS= read -r new_module || [ -n "$new_module" ]; do
        [ -z "$new_module" ] && continue
        ((CURRENT_INDEX++))
        show_progress "$CURRENT_INDEX" "$NEW_MODULE_COUNT" "Generating load order" "$new_module"
        
        dependents=$(grep -w -- "$new_module" modules.dep | cut -d: -f1 | sed 's/^\.\///')
        insertion_line=99999

        if [ -n "$dependents" ]; then
            first_dependent_line=$(echo "$dependents" | xargs -I {} grep -n "^{}$" modules.load.final | head -1 | cut -d: -f1)
            [ -n "$first_dependent_line" ] && insertion_line=$first_dependent_line
        fi
        echo -e "$insertion_line\t$new_module" >> insertions.tsv
    done < new_modules.list
    finish_progress

    # Insert modules based on dependencies
    while IFS=$'\t' read -r line_num module_to_insert; do
        if [ "$line_num" -eq 99999 ]; then
            echo "$module_to_insert" >> modules.load.final
        else
            sed -i "${line_num}i$module_to_insert" modules.load.final
        fi
    done < <(sort -r -n insertions.tsv)
    log_info "Inserted $NEW_MODULE_COUNT new modules into load order"
fi

# Filter out dependency modules from NEW modules only (preserve OEM modules.load as-is)
# modprobe will automatically load dependencies, so we don't need to list them for new modules
print_header "Filtering Dependency Modules from New Modules"

# Create a map of all modules that are dependencies
> dependencies_map.txt
while IFS=':' read -r module deps_line || [ -n "$module" ]; do
    [ -z "$module" ] && continue
    clean_module=$(basename "$module")
    
    if [ -n "$deps_line" ]; then
        clean_deps=$(echo "$deps_line" | sed 's/^ *//' | sed 's/ *$//')
        if [ -n "$clean_deps" ]; then
            for dep in $clean_deps; do
                clean_dep=$(basename "$dep")
                echo "$clean_dep" >> dependencies_map.txt
            done
        fi
    fi
done < modules.dep

# Filter dependency modules from NEW modules only, preserving relative order
# OEM modules are preserved as-is, new modules are filtered inline
> modules.load.filtered
FILTERED_COUNT=0
NEW_TOTAL_COUNT=0

while IFS= read -r module_name || [ -n "$module_name" ]; do
    [ -z "$module_name" ] && continue
    
    # Check if this module is from OEM list
    if grep -Fxq "$module_name" oem_modules.list 2>/dev/null; then
        # OEM module - preserve as-is (keep original order)
        echo "$module_name" >> modules.load.filtered
    else
        # New module - count it and filter out if it's a dependency
        ((NEW_TOTAL_COUNT++))
        if ! grep -Fxq "$module_name" dependencies_map.txt 2>/dev/null; then
            # This is a leaf module (not a dependency) - keep it in original position
            echo "$module_name" >> modules.load.filtered
        else
            # This is a dependency module - filter it out
            ((FILTERED_COUNT++))
        fi
    fi
done < modules.load.final

mv modules.load.filtered modules.load

if [ $FILTERED_COUNT -gt 0 ]; then
    NEW_FILTERED_COUNT=$((NEW_TOTAL_COUNT - FILTERED_COUNT))
    log_info "Filtered $FILTERED_COUNT dependency modules from new modules (kept $NEW_FILTERED_COUNT leaf modules)"
    log_info "OEM modules.load entries preserved as-is with original ordering"
    log_info "modprobe will automatically load dependencies when needed"
fi

rm -f our_modules.list oem_modules.list modules.load.base new_modules.list insertions.tsv dependencies_map.txt modules.load.final

# --- Copy Final Results ---

print_header "Finalizing Output"
cp *.ko modules.* "$OUTPUT_DIR/" 2>/dev/null
log_info "Output files copied"

# --- Summary ---

FINAL_COUNT=$(ls -1 "$OUTPUT_DIR"/*.ko 2>/dev/null | wc -l)
LOAD_COUNT=$(wc -l < "$OUTPUT_DIR/modules.load" 2>/dev/null || echo "0")

print_header "Process Complete!"
echo "Vendor DLKM module preparation successful!"
echo ""
echo "Results:"
echo "  - Total final modules: $FINAL_COUNT"
echo "  - Load order entries:  $LOAD_COUNT"
echo "  - NetHunter modules added: $NH_COUNT"
echo "  - Vendor boot modules pruned: $PRUNED_COUNT"
echo "  - Output directory:      $OUTPUT_DIR"
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

echo "Your vendor_dlkm module set is ready for packaging!"
