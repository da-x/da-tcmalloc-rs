use std::{ffi::CString, path::PathBuf};

pub use tcmalloc_sys::HeapProfilerVars;

pub fn start(path: PathBuf) {
    let cstr_path = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
    unsafe {
        tcmalloc_sys::HeapProfilerStart(cstr_path.as_ptr());
    }
}

pub fn set_exact_path(path: PathBuf) {
    let cstr_path = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
    unsafe {
        tcmalloc_sys::HeapProfilerSetExactPath(cstr_path.as_ptr());
    }
}

pub fn dump(s: impl AsRef<str>) {
    let cstr_path = CString::new(s.as_ref()).unwrap();
    unsafe {
        tcmalloc_sys::HeapProfilerDump(cstr_path.as_ptr());
    }
}

pub fn stop() {
    unsafe {
        tcmalloc_sys::HeapProfilerStop();
    }
}

pub fn set_vars(vars: &HeapProfilerVars) {
    unsafe {
        tcmalloc_sys::HeapProfilerSetVars(vars);
    }
}
