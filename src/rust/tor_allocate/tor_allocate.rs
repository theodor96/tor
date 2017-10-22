use libc::{c_char, c_void};
use std::{ptr, slice};

#[cfg(not(test))]
extern "C" {
    fn tor_malloc_ ( size: usize) ->  *mut c_void;
}

// Defined only for tests, used for testing purposes, so that we don't need
// to link to tor C files. Uses the system allocator
#[cfg(test)]
extern "C" fn tor_malloc_ ( size: usize) ->  *mut c_void {
    use libc::malloc;
    unsafe { malloc(size) }
}

/// Allocate memory using tor_malloc_ and copy an existing string into the
/// allocated buffer, returning a pointer that can later be called in C.
///
/// # Inputs
///
/// * `src`, a reference to a String that will be copied.
///
/// # Returns
///
/// A `String` that should be freed by tor_free in C
///
pub fn allocate_and_copy_string(src: &String) -> *mut c_char {
    let bytes = src.as_bytes();

    let size = bytes.len();
    let size_with_null_byte = size + 1;

    let dest = unsafe { tor_malloc_(size_with_null_byte) as *mut u8 };

    if dest.is_null() {
        return dest as *mut c_char;
    }

    unsafe { ptr::copy_nonoverlapping(bytes.as_ptr(), dest, size) };

    // set the last byte as null, using the ability to index into a slice
    // rather than doing pointer arithmatic
    let slice = unsafe { slice::from_raw_parts_mut(dest, size_with_null_byte) };
    slice[size] = 0; // add a null terminator

    dest as *mut c_char
}

#[cfg(test)]
mod test {

    #[test]
    fn test_allocate_and_copy_string_with_empty() {
        use std::ffi::CStr;
        use libc::{free, c_void};

        use tor_allocate::allocate_and_copy_string;

        let empty = String::new();
        let allocated_empty = allocate_and_copy_string(&empty);

        let allocated_empty_rust = unsafe {
            CStr::from_ptr(allocated_empty).to_str().unwrap()
        };

        assert_eq!("", allocated_empty_rust);

        unsafe { free(allocated_empty as *mut c_void) };
    }

    #[test]
    fn test_allocate_and_copy_string_with_not_empty_string() {
        use std::ffi::CStr;
        use libc::{free, c_void};

        use tor_allocate::allocate_and_copy_string;

        let empty = String::from("foo bar biz");
        let allocated_empty = allocate_and_copy_string(&empty);

        let allocated_empty_rust = unsafe {
            CStr::from_ptr(allocated_empty).to_str().unwrap()
        };

        assert_eq!("foo bar biz", allocated_empty_rust);

        unsafe { free(allocated_empty as *mut c_void) };
    }
}
