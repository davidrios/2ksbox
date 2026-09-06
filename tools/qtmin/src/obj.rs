//! One QObject, as small as cxx-qt lets one be: a single string property.
#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[cfg_attr(feature = "qml", qml_element)]
        #[qproperty(QString, label)]
        type Thing = super::ThingRust;
    }
}

pub struct ThingRust {
    label: cxx_qt_lib::QString,
}

impl Default for ThingRust {
    fn default() -> Self {
        Self { label: cxx_qt_lib::QString::from("qtmin") }
    }
}
