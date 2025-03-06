use std::{ffi::{c_char, c_int, c_void, CString}, path::PathBuf};

pub use da_tcmalloc_sys::HeapProfilerVars;
use da_tcmalloc_sys::{MallocExtension_GetAllocatedSize, MallocExtension_GetEstimatedAllocatedSize, MallocExtension_GetMemoryReleaseRate, MallocExtension_GetNumericProperty, MallocExtension_GetStats, MallocExtension_GetThreadCacheSize, MallocExtension_MallocMemoryStats, MallocExtension_MarkThreadBusy, MallocExtension_MarkThreadIdle, MallocExtension_MarkThreadTemporarilyIdle, MallocExtension_ReleaseFreeMemory, MallocExtension_ReleaseToSystem, MallocExtension_SetMemoryReleaseRate, MallocExtension_SetNumericProperty, MallocExtension_VerifyAllMemory, MallocExtension_VerifyArrayNewMemory, MallocExtension_VerifyMallocMemory, MallocExtension_VerifyNewMemory};

pub fn start(path: PathBuf) {
    let cstr_path = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
    unsafe {
        da_tcmalloc_sys::HeapProfilerStart(cstr_path.as_ptr());
    }
}

pub fn set_exact_path(path: PathBuf) {
    let cstr_path = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
    unsafe {
        da_tcmalloc_sys::HeapProfilerSetExactPath(cstr_path.as_ptr());
    }
}

pub fn dump(s: impl AsRef<str>) {
    let cstr_path = CString::new(s.as_ref()).unwrap();
    unsafe {
        da_tcmalloc_sys::HeapProfilerDump(cstr_path.as_ptr());
    }
}

pub fn stop() {
    unsafe {
        da_tcmalloc_sys::HeapProfilerStop();
    }
}

pub fn set_vars(vars: &HeapProfilerVars) {
    unsafe {
        da_tcmalloc_sys::HeapProfilerSetVars(vars);
    }
}

/// Verifies all memory. Returns the result code.
pub fn verify_all_memory() -> i32 {
    unsafe { MallocExtension_VerifyAllMemory() }
}

/// Verifies memory allocated via operator new.
/// Accepts a reference to a value and passes its pointer.
pub fn verify_new_memory<T>(p: &T) -> i32 {
    unsafe { MallocExtension_VerifyNewMemory(p as *const T as *const c_void) }
}

/// Verifies memory allocated via operator new[].
/// Accepts a slice and passes its pointer.
pub fn verify_array_new_memory<T>(p: &[T]) -> i32 {
    unsafe { MallocExtension_VerifyArrayNewMemory(p.as_ptr() as *const c_void) }
}

/// Verifies memory allocated via malloc.
pub fn verify_malloc_memory<T>(p: &T) -> i32 {
    unsafe { MallocExtension_VerifyMallocMemory(p as *const T as *const c_void) }
}

/// Retrieves memory statistics.
///
/// # Parameters
/// - `blocks`: mutable reference to a c_int for the number of blocks.
/// - `total`: mutable reference to a usize for the total size.
/// - `histogram`: mutable reference to a c_int for histogram data.
///
/// Returns the result code.
pub fn malloc_memory_stats(
    blocks: &mut i32,
    total: &mut usize,
    histogram: &mut i32,
) -> i32 {
    unsafe {
        MallocExtension_MallocMemoryStats(
            blocks as *mut i32,
            total as *mut usize,
            histogram as *mut i32,
        )
    }
}

/// Fills the given buffer with stats.
///
/// # Safety Note
/// The buffer must be large enough to hold the stats.
pub fn get_stats(buffer: &mut [u8]) {
    unsafe {
        MallocExtension_GetStats(buffer.as_mut_ptr() as *mut c_char, buffer.len() as c_int)
    }
}

/// Gets a numeric property.
///
/// Returns Ok(value) on success or Err(error_code) on failure.
pub fn get_numeric_property(property: &str) -> Result<usize, i32> {
    let c_property = CString::new(property).expect("CString conversion failed");
    let mut value: usize = 0;
    let ret = unsafe {
        MallocExtension_GetNumericProperty(c_property.as_ptr(), &mut value)
    };
    if ret == 0 {
        Ok(value)
    } else {
        Err(ret)
    }
}

/// Sets a numeric property.
///
/// Returns Ok(()) on success or Err(error_code) on failure.
pub fn set_numeric_property(property: &str, value: usize) -> Result<(), i32> {
    let c_property = CString::new(property).expect("CString conversion failed");
    let ret = unsafe { MallocExtension_SetNumericProperty(c_property.as_ptr(), value) };
    if ret == 0 {
        Ok(())
    } else {
        Err(ret)
    }
}

/// Marks the current thread as idle.
pub fn mark_thread_idle() {
    unsafe { MallocExtension_MarkThreadIdle() }
}

/// Marks the current thread as busy.
pub fn mark_thread_busy() {
    unsafe { MallocExtension_MarkThreadBusy() }
}

/// Releases memory back to the system.
pub fn release_to_system(num_bytes: usize) {
    unsafe { MallocExtension_ReleaseToSystem(num_bytes) }
}

/// Releases free memory.
pub fn release_free_memory() {
    unsafe { MallocExtension_ReleaseFreeMemory() }
}

/// Sets the rate at which memory is released.
pub fn set_memory_release_rate(rate: f64) {
    unsafe { MallocExtension_SetMemoryReleaseRate(rate) }
}

/// Gets the current memory release rate.
pub fn get_memory_release_rate() -> f64 {
    unsafe { MallocExtension_GetMemoryReleaseRate() }
}

/// Gets the estimated allocated size for a given allocation size.
pub fn get_estimated_allocated_size(size: usize) -> usize {
    unsafe { MallocExtension_GetEstimatedAllocatedSize(size) }
}

/// Gets the allocated size for a given pointer.
pub fn get_allocated_size<T>(p: *const T) -> usize {
    unsafe { MallocExtension_GetAllocatedSize(p as *const c_void) }
}

/// Retrieves the size of the thread's cache.
pub fn get_thread_cache_size() -> usize {
    unsafe { MallocExtension_GetThreadCacheSize() }
}

/// Temporarily marks the current thread as idle.
pub fn mark_thread_temporarily_idle() {
    unsafe { MallocExtension_MarkThreadTemporarilyIdle() }
}
