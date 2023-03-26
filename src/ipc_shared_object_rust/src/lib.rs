autocxx::include_cpp! {
    #include "boost/interprocess/mapped_region.hpp"
    #include "boost/interprocess/shared_memory_object.hpp"
    generate!("boost::interprocess::mapped_region")
    generate!("boost::interprocess::mode_t")
    generate!("boost::interprocess::open_only_t")
    generate!("boost::interprocess::shared_memory_object")
}

cpp::cpp! {{
    #include "boost/interprocess/mapped_region.hpp"
    #include "boost/interprocess/shared_memory_object.hpp"

    namespace ipc = boost::interprocess;
}}

pub struct IpcUnmanagedObject<T> {
    phantom_data: core::marker::PhantomData<T>,
    _object: ffi::boost::interprocess::shared_memory_object,
    region: ffi::boost::interprocess::mapped_region,
}

impl<T> IpcUnmanagedObject<T> {
    pub fn new(name: &str) -> IpcUnmanagedObject<T> {
        unsafe {
            let cname_storage = std::ffi::CString::new(name).unwrap();
            let cname = cname_storage.as_ptr();

            let object = cpp::cpp!([cname as "char const *"]
                                   -> ffi::boost::interprocess::shared_memory_object
                                   as "ipc::shared_memory_object" {
                return ipc::shared_memory_object{ipc::open_only, cname, ipc::read_write};
            });

            let size = core::mem::size_of::<T>();

            let region = cpp::cpp!([object as "ipc::shared_memory_object", size as "std::size_t"]
                                   -> ffi::boost::interprocess::mapped_region
                                   as "ipc::mapped_region" {
                return ipc::mapped_region{object, ipc::read_write, 0, size};
            });

            IpcUnmanagedObject {
                phantom_data: core::marker::PhantomData,
                _object: object,
                region: region,
            }
        }
    }

    pub fn get_ref<'a>(&'a self) -> &'a T {
        unsafe { core::mem::transmute::<*const autocxx::c_void, &'a T>(self.region.get_address()) }
    }

    pub fn get_mut<'a>(&'a mut self) -> core::pin::Pin<&'a mut T> {
        unsafe {
            core::pin::Pin::new_unchecked(
                core::mem::transmute::<*const autocxx::c_void, &'a mut T>(
                    self.region.get_address(),
                ),
            )
        }
    }
}
