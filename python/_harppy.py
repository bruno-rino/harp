"""Copyright (C) 2015-2016 S[&]T, The Netherlands.

This file is part of HARP.

HARP is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation;
either version 2 of the License, or (at your option) any later version.

HARP is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
HARP; if not, write to the Free Software Foundation, Inc., 59 Temple Place,
Suite 330, Boston, MA  02111-1307  USA

"""

from __future__ import print_function

from collections import OrderedDict
import numpy

try:
    from cStringIO import StringIO
except ImportError:
    try:
        from StringIO import StringIO
    except ImportError:
        from io import StringIO

from harp._harpc import ffi as _ffi

__all__ = ["Error", "CLibraryError", "UnsupportedTypeError", "UnsupportedDimensionError", "Variable", "Product",
           "get_encoding", "set_encoding", "version", "ingest_product", "import_product", "export_product", "to_dict"]

class Error(Exception):
    """Exception base class for all HARP Python interface errors."""
    pass

class CLibraryError(Error):
    """Exception raised when an error occurs inside the HARP C library.

    Attributes:
        errno       --  error code; if None, the error code will be retrieved from
                        the HARP C library.
        strerror    --  error message; if None, the error message will be retrieved
                        from the HARP C library.

    """
    def __init__(self, errno=None, strerror=None):
        if errno is None:
            errno = _lib.harp_errno

        if strerror is None:
            strerror = _decode_string(_ffi.string(_lib.harp_errno_to_string(errno)))

        super(CLibraryError, self).__init__(errno, strerror)
        self.errno = errno
        self.strerror = strerror

    def __str__(self):
        return self.strerror

class UnsupportedTypeError(Error):
    """Exception raised when unsupported types are encountered, either on the Python
    or on the C side of the interface.

    """
    pass

class UnsupportedDimensionError(Error):
    """Exception raised when unsupported dimensions are encountered, either on the
    Python or on the C side of the interface.

    """
    pass

class NoDataError(Error):
    """Exception raised when the product returned from an ingestion or import
    contains no variables, or variables without data.

    """
    def __init__(self):
        super(NoDataError, self).__init__("product contains no variables, or variables without data")

class Variable(object):
    """Python representation of a HARP variable.

    A variable consists of data (either a scalar or NumPy array), a list of
    dimension types that describe the dimensions of the data, and a number of
    optional attributes: physical unit, minimum valid value, maximum valid
    value, and a human-readable description.

    """
    def __init__(self, data, dimension=[], unit=None, valid_min=None, valid_max=None, description=None):
        self.data = data
        self.dimension = dimension

        if unit:
            self.unit = unit
        if valid_min:
            self.valid_min = valid_min
        if valid_max:
            self.valid_max = valid_max
        if description:
            self.description = description

    def __repr__(self):
        if not self.dimension:
            return "<Variable type=%s>" % _format_data_type(self.data)

        return "<Variable type=%s dimension=%s>" % (_format_data_type(self.data),
                                                    _format_dimensions(self.dimension, self.data))

    def __str__(self):
        stream = StringIO()

        print("type =", _format_data_type(self.data), file=stream)

        if self.dimension:
            print("dimension =", _format_dimensions(self.dimension, self.data), file=stream)

        try:
            unit = self.unit
        except AttributeError:
            pass
        else:
            if unit:
                print("unit = %r" % unit, file=stream)

        try:
            valid_min = self.valid_min
        except AttributeError:
            pass
        else:
            if valid_min is not None:
                print("valid_min = %r" % valid_min, file=stream)

        try:
            valid_max = self.valid_max
        except AttributeError:
            pass
        else:
            if valid_max is not None:
                print("valid_max = %r" % valid_max, file=stream)

        try:
            description = self.description
        except AttributeError:
            pass
        else:
            if description:
                print("description = %r" % description, file=stream)

        if self.data is not None:
            if not isinstance(self.data, numpy.ndarray) and not numpy.isscalar(self.data):
                print("data = <invalid>", file=stream)
            elif numpy.isscalar(self.data):
                print("data = %r" % self.data, file=stream)
            elif not self.dimension and self.data.size == 1:
                print("data = %r" % self.data.flat[0], file=stream)
            elif self.data.size == 0:
                print("data = <empty>", file=stream)
            else:
                print("data =", file=stream)
                print(str(self.data), file=stream)

        return stream.getvalue()

class Product(object):
    """Python representation of a HARP product.

    A product consists of product attributes and variables. Any attribute of a
    Product instance of which the name does not start with an underscore is either
    a variable or a product attribute. Product attribute names are reserved and
    cannot be used for variables.

    The list of names reserved for product attributes is:
        source_product  --  Name of the original product this product is derived
                            from.
        history         --  New-line separated list of invocations of HARP command
                            line tools that have been performed on the product.
    """
    # Product attribute names. All attribute names of this class that do not start with an underscore are assumed to be
    # HARP variable names, except for the reserved names listed below.
    _reserved_names = set(("source_product", "history"))

    def __init__(self, source_product="", history=""):
        if source_product:
            self.source_product = source_product
        if history:
            self.history = history
        self._variable_dict = OrderedDict()

    def _is_reserved_name(self, name):
        return name.startswith("_") or name in Product._reserved_names

    def _verify_key(self, key):
        if not isinstance(key, str):
            # The statement obj.__class__.__name__ works for both new-style and old-style classes.
            raise TypeError("key must be str, not %r" % key.__class__.__name__)

        if self._is_reserved_name(key):
            raise KeyError(key)

    def __setattr__(self, name, value):
        super(Product, self).__setattr__(name, value)
        if not self._is_reserved_name(name):
            self._variable_dict[str(name)] = value

    def __delattr__(self, name):
        super(Product, self).__delattr__(name)
        if not self._is_reserved_name(name):
            del self._variable_dict[str(name)]

    def __getitem__(self, key):
        self._verify_key(key)
        try:
            return getattr(self, key)
        except AttributeError:
            raise KeyError(key)

    def __setitem__(self, key, value):
        self._verify_key(key)
        setattr(self, key, value)

    def __delitem__(self, key):
        self._verify_key(key)
        try:
            delattr(self, key)
        except AttributeError:
            raise KeyError(key)

    def __len__(self):
        return len(self._variable_dict)

    def __iter__(self):
        return iter(self._variable_dict)

    def __reversed__(self):
        return reversed(self._variable_dict)

    def __contains__(self, name):
        return name in self._variable_dict

    def __repr__(self):
        return "<Product variables=%r>" % self._variable_dict.keys()

    def __str__(self):
        stream = StringIO()

        # Attributes.
        has_attributes = False

        try:
            source_product = self.source_product
        except AttributeError:
            pass
        else:
            if source_product:
                print("source product = %r" % source_product, file=stream)
                has_attributes = True

        try:
            history = self.history
        except AttributeError:
            pass
        else:
            if history:
                print("history = %r" % history, file=stream)
                has_attributes = True

        # Variables.
        if not self._variable_dict:
            return stream.getvalue()

        if has_attributes:
            stream.write("\n")

        for name, variable in _dict_iteritems(self._variable_dict):
            if not isinstance(variable, Variable):
                print("<non-compliant variable %r>" % name, file=stream)
                continue

            if not isinstance(variable.data, numpy.ndarray) and not numpy.isscalar(variable.data):
                print("<non-compliant variable %r>" % name, file=stream)
                continue

            if isinstance(variable.data, numpy.ndarray) and variable.data.size == 0:
                print("<empty variable %r>" % name, file=stream)
                continue

            # Data type and variable name.
            stream.write(_format_data_type(variable.data) + " " + name)

            # Dimensions.
            if variable.dimension:
                stream.write(" " + _format_dimensions(variable.dimension, variable.data))

            # Unit.
            try:
                unit = variable.unit
            except AttributeError:
                pass
            else:
                if unit:
                    stream.write(" [%s]" % unit)

            stream.write("\n")

        return stream.getvalue()

def _get_c_library_filename():
    """Return the filename of the HARP shared library depending on the current
    platform.

    """
    from platform import system as _system

    if _system() == "Windows":
        return "harp.dll"
    if _system() == "Darwin":
        import os.path
        path = os.path.join(os.path.dirname(__file__), "../../..")
        return os.path.join(os.path.normpath(path), "libharp.dylib")
    return "libharp.so"

def _get_filesystem_encoding():
    """Return the encoding used by the filesystem."""
    from sys import getdefaultencoding as _getdefaultencoding, getfilesystemencoding as _getfilesystemencoding

    encoding = _getfilesystemencoding()
    if encoding is None:
        encoding = _getdefaultencoding()

    return encoding

def _init():
    """Initialize the HARP Python interface."""
    global _lib, _encoding, _py_dimension_type, _c_dimension_type, _py_data_type, _c_data_type_name

    # Initialize the HARP C library
    _lib = _ffi.dlopen(_get_c_library_filename())

    if _lib.harp_init() != 0:
        raise CLibraryError()

    # Set default encoding.
    _encoding = "ascii"

    # Initialize various look-up tables used thoughout the HARP Python interface (i.e. this module).
    _py_dimension_type = \
        {
            _lib.harp_dimension_independent: None,
            _lib.harp_dimension_time: "time",
            _lib.harp_dimension_latitude: "latitude",
            _lib.harp_dimension_longitude: "longitude",
            _lib.harp_dimension_vertical: "vertical",
            _lib.harp_dimension_spectral: "spectral"
        }

    _c_dimension_type = \
        {
            None: _lib.harp_dimension_independent,
            "time": _lib.harp_dimension_time,
            "latitude": _lib.harp_dimension_latitude,
            "longitude": _lib.harp_dimension_longitude,
            "vertical": _lib.harp_dimension_vertical,
            "spectral": _lib.harp_dimension_spectral
        }

    _py_data_type = \
        {
            _lib.harp_type_int8: numpy.int8,
            _lib.harp_type_int16: numpy.int16,
            _lib.harp_type_int32: numpy.int32,
            _lib.harp_type_float: numpy.float32,
            _lib.harp_type_double: numpy.float64,
            _lib.harp_type_string: numpy.object_
        }

    _c_data_type_name = \
        {
            _lib.harp_type_int8: "byte",
            _lib.harp_type_int16: "int",
            _lib.harp_type_int32: "long",
            _lib.harp_type_float: "float",
            _lib.harp_type_double: "double",
            _lib.harp_type_string: "string"
        }

def _all(predicate, sequence):
    """Return True if the predicate evaluates to True for all elements in the
    sequence, False otherwise.

    Attributes:
        predicate   --  Predicate to use; this should be a callable that takes a
                        single argument and returns a bool.
        sequence    --  Sequence to test.

    """
    return reduce(lambda x, y: x and predicate(y), sequence, True)

def _dict_iteritems(dictionary):
    """Get an iterator or view on the items of the specified dictionary.

    This method is Python 2 and Python 3 compatible.
    """
    try:
        return dictionary.iteritems()
    except AttributeError:
        return dictionary.items()

def _get_py_dimension_type(dimension_type):
    """Return the dimension name corresponding to the specified C dimension type
    code.

    """
    try:
        return _py_dimension_type[dimension_type]
    except KeyError:
        raise UnsupportedDimensionError("unsupported C dimension type code '%d'" % dimension_type)

def _get_c_dimension_type(dimension_type):
    """Return the C dimension type code corresponding to the specified dimension
    name.

    """
    try:
        return _c_dimension_type[dimension_type]
    except KeyError:
        raise UnsupportedDimensionError("unsupported dimension %r" % dimension_type)

def _get_py_data_type(data_type):
    """Return the Python type corresponding to the specified C data type code."""
    try:
        return _py_data_type[data_type]
    except KeyError:
        raise UnsupportedTypeError("unsupported C data type code '%d'" % data_type)

def _get_c_data_type(value):
    """Return the C data type code corresponding to the specified variable data
    value.

    """
    if isinstance(value, (numpy.ndarray, numpy.generic)):
        # For NumPy ndarrays and scalars, determine the smallest HARP C data type that can safely contain elements of
        # the ndarray or scalar dtype.
        if numpy.issubdtype(value.dtype, numpy.object_):
            # NumPy object arrays are only used to contain variable length strings or byte strings.
            if _all(lambda element: isinstance(element, str), value.flat):
                return _lib.harp_type_string
            elif _all(lambda element: isinstance(element, bytes), value.flat):
                return _lib.harp_type_string
            else:
                raise UnsupportedTypeError("elements of a NumPy object array must be all str or all bytes")
        elif numpy.issubdtype(value.dtype, numpy.str_) or numpy.issubdtype(value.dtype, numpy.bytes_):
            return _lib.harp_type_string
        elif numpy.can_cast(value.dtype, numpy.int8):
            return _lib.harp_type_int8
        elif numpy.can_cast(value.dtype, numpy.int16):
            return _lib.harp_type_int16
        elif numpy.can_cast(value.dtype, numpy.int32):
            return _lib.harp_type_int32
        elif numpy.can_cast(value.dtype, numpy.float32):
            return _lib.harp_type_float
        elif numpy.can_cast(value.dtype, numpy.float64):
            return _lib.harp_type_double
        else:
            raise UnsupportedTypeError("unsupported NumPy dtype '%s'" % value.dtype)
    elif isinstance(value, (str, bytes)):
        return _lib.harp_type_string
    elif numpy.can_cast(value, numpy.int8):
        return _lib.harp_type_int8
    elif numpy.can_cast(value, numpy.int16):
        return _lib.harp_type_int16
    elif numpy.can_cast(value, numpy.int32):
        return _lib.harp_type_int32
    elif numpy.can_cast(value, numpy.float32):
        return _lib.harp_type_float
    elif numpy.can_cast(value, numpy.float64):
        return _lib.harp_type_double
    else:
        raise UnsupportedTypeError("unsupported type %r" % value.__class__.__name__)

def _get_c_data_type_name(data_type):
    """Return the canonical name for the specified C data type code."""
    try:
        return _c_data_type_name[data_type]
    except KeyError:
        raise UnsupportedTypeError("unsupported C data type code '%d'" % data_type)

def _c_can_cast(c_data_type_src, c_data_type_dst):
    """Returns True if the source C data type can be cast to the destination C data
    type while preserving values.

    """
    if c_data_type_dst == _lib.harp_type_int8:
        return (c_data_type_src == _lib.harp_type_int8)
    elif c_data_type_dst == _lib.harp_type_int16:
        return (c_data_type_src == _lib.harp_type_int8 or
                c_data_type_src == _lib.harp_type_int16)
    elif c_data_type_dst == _lib.harp_type_int32:
        return (c_data_type_src == _lib.harp_type_int8 or
                c_data_type_src == _lib.harp_type_int16 or
                c_data_type_src == _lib.harp_type_int32)
    elif c_data_type_dst == _lib.harp_type_float:
        return (c_data_type_src == _lib.harp_type_int8 or
                c_data_type_src == _lib.harp_type_int16 or
                c_data_type_src == _lib.harp_type_float)
    elif c_data_type_dst == _lib.harp_type_double:
        return (c_data_type_src == _lib.harp_type_int8 or
                c_data_type_src == _lib.harp_type_int16 or
                c_data_type_src == _lib.harp_type_int32 or
                c_data_type_src == _lib.harp_type_float or
                c_data_type_src == _lib.harp_type_double)
    elif c_data_type_dst == _lib.harp_type_string:
        return (c_data_type_src == _lib.harp_type_string)
    else:
        return False

def _encode_string_with_encoding(string, encoding="utf-8"):
    """Encode a unicode string using the specified encoding.

    By default, use the "surrogateescape" error handler to deal with encoding
    errors. This error handler ensures that invalid bytes encountered during
    decoding are converted to the same bytes during encoding, by decoding them
    to a special range of unicode code points.

    The "surrogateescape" error handler is available since Python 3.1. For earlier
    versions of Python 3, the "strict" error handler is used instead.

    """
    try:
        try:
            return string.encode(encoding, "surrogateescape")
        except LookupError:
            # Either the encoding or the error handler is not supported; fall-through to the next try-block.
            pass

        try:
            return string.encode(encoding)
        except LookupError:
            # Here it is certain that the encoding is not supported.
            raise Error("unknown encoding '%s'" % encoding)
    except UnicodeEncodeError:
        raise Error("cannot encode '%s' using encoding '%s'" % (string, encoding))

def _decode_string_with_encoding(string, encoding="utf-8"):
    """Decode a byte string using the specified encoding.

    By default, use the "surrogateescape" error handler to deal with encoding
    errors. This error handler ensures that invalid bytes encountered during
    decoding are converted to the same bytes during encoding, by decoding them
    to a special range of unicode code points.

    The "surrogateescape" error handler is available since Python 3.1. For earlier
    versions of Python 3, the "strict" error handler is used instead. This may cause
    decoding errors if the input byte string contains bytes that cannot be decoded
    using the specified encoding. Since most HARP products use ASCII strings
    exclusively, it is unlikely this will occur often in practice.

    """
    try:
        try:
            return string.decode(encoding, "surrogateescape")
        except LookupError:
            # Either the encoding or the error handler is not supported; fall-through to the next try-block.
            pass

        try:
            return string.decode(encoding)
        except LookupError:
            # Here it is certain that the encoding is not supported.
            raise Error("unknown encoding '%s'" % encoding)
    except UnicodeEncodeError:
        raise Error("cannot decode '%s' using encoding '%s'" % (string, encoding))

def _encode_path(path):
    """Encode the input unicode path using the filesystem encoding.

    On Python 2, this method returns the specified path unmodified.

    """
    if isinstance(path, bytes):
        # This branch will be taken for instances of class str on Python 2 (since this is an alias for class bytes), and
        # on Python 3 for instances of class bytes.
        return path
    elif isinstance(path, str):
        # This branch will only be taken for instances of class str on Python 3. On Python 2 such instances will take
        # the branch above.
        return _encode_string_with_encoding(path, _get_filesystem_encoding())
    else:
        raise TypeError("path must be bytes or str, not %r" % path.__class__.__name__)

def _encode_string(string):
    """Encode the input unicode string using the package default encoding.

    On Python 2, this method returns the specified string unmodified.

    """
    if isinstance(string, bytes):
        # This branch will be taken for instances of class str on Python 2 (since this is an alias for class bytes), and
        # on Python 3 for instances of class bytes.
        return string
    elif isinstance(string, str):
        # This branch will only be taken for instances of class str on Python 3. On Python 2 such instances will take
        # the branch above.
        return _encode_string_with_encoding(string, get_encoding())
    else:
        raise TypeError("string must be bytes or str, not %r" % string.__class__.__name__)

def _decode_string(string):
    """Decode the input byte string using the package default encoding.

    On Python 2, this method returns the specified byte string unmodified.

    """
    if isinstance(string, str):
        # This branch will be taken for instances of class str on Python 2 and Python 3.
        return string
    elif isinstance(string, bytes):
        # This branch will only be taken for instances of class bytes on Python 3. On Python 2 such instances will take
        # the branch above.
        return _decode_string_with_encoding(string, get_encoding())
    else:
        raise TypeError("string must be bytes or str, not %r" % string.__class__.__name__)

def _format_data_type(data):
    """Return the string representation of the C data type that would be used to
    store the specified data, or "<invalid>" if the specified data is of an
    unsupported type.

    """
    try:
        return _get_c_data_type_name(_get_c_data_type(data))
    except UnsupportedTypeError:
        return "<invalid>"

def _format_dimensions(dimension, data):
    """Construct a formatted string from the specified dimensions and data that
    provides information about dimension types and lengths, or "<invalid>" if this
    information cannot be determined.

    """
    if not isinstance(data, numpy.ndarray) or data.ndim != len(dimension):
        return "{<invalid>}"

    stream = StringIO()

    stream.write("{")
    for i in range(data.ndim):
        if dimension[i]:
            stream.write(dimension[i] + "=")
        stream.write(str(data.shape[i]))

        if (i + 1) < data.ndim:
            stream.write(", ")
    stream.write("}")

    return stream.getvalue()

def _import_scalar(c_data_type, c_data):
    if c_data_type == _lib.harp_type_int8:
        return c_data.int8_data
    elif c_data_type == _lib.harp_type_int16:
        return c_data.int16_data
    elif c_data_type == _lib.harp_type_int32:
        return c_data.int32_data
    elif c_data_type == _lib.harp_type_float:
        return c_data.float_data
    elif c_data_type == _lib.harp_type_double:
        return c_data.double_data

    raise UnsupportedTypeError("unsupported C data type code '%d'" % c_data_type)

def _import_array(c_data_type, c_num_elements, c_data):
    if c_data_type == _lib.harp_type_string:
        data = numpy.empty((c_num_elements,), dtype=numpy.object)
        for i in range(c_num_elements):
            # NB. The _ffi.string() method returns a copy of the C string.
            data[i] = _decode_string(_ffi.string(c_data.string_data[i]))

        return data

    # NB. The _ffi.buffer() method, as well as the numpy.frombuffer() method, provide a view on the C array; neither
    # method incurs a copy.
    c_data_buffer = _ffi.buffer(c_data.ptr, c_num_elements * _lib.harp_get_size_for_type(c_data_type))
    return numpy.copy(numpy.frombuffer(c_data_buffer, dtype=_get_py_data_type(c_data_type)))

def _import_variable(c_variable):
    # Import variable data.
    data = _import_array(c_variable.data_type, c_variable.num_elements, c_variable.data)

    num_dimensions = c_variable.num_dimensions
    if num_dimensions == 0:
        variable = Variable(numpy.asscalar(data))
    else:
        data = data.reshape([c_variable.dimension[i] for i in range(num_dimensions)])
        dimension = [_get_py_dimension_type(c_variable.dimension_type[i]) for i in range(num_dimensions)]
        variable = Variable(data, dimension)

    # Import variable attributes.
    if c_variable.unit:
        variable.unit = _decode_string(_ffi.string(c_variable.unit))

    if c_variable.data_type != _lib.harp_type_string:
        variable.valid_min = _import_scalar(c_variable.data_type, c_variable.valid_min)
        variable.valid_max = _import_scalar(c_variable.data_type, c_variable.valid_max)

    if c_variable.description:
        variable.description = _decode_string(_ffi.string(c_variable.description))

    return variable

def _import_product(c_product):
    product = Product()

    # Import product attributes.
    if c_product.source_product:
        product.source_product = _decode_string(_ffi.string(c_product.source_product))

    if c_product.history:
        product.history = _decode_string(_ffi.string(c_product.history))

    # Import variables.
    for i in range(c_product.num_variables):
        c_variable_ptr = c_product.variable[i]
        variable = _import_variable(c_variable_ptr[0])
        setattr(product, _decode_string(_ffi.string(c_variable_ptr[0].name)), variable)

    return product

def _export_scalar(data, c_data_type, c_data):
    if c_data_type == _lib.harp_type_int8:
        c_data.int8_data = data
    elif c_data_type == _lib.harp_type_int16:
        c_data.int16_data = data
    elif c_data_type == _lib.harp_type_int32:
        c_data.int32_data = data
    elif c_data_type == _lib.harp_type_float:
        c_data.float_data = data
    elif c_data_type == _lib.harp_type_double:
        c_data.double_data = data
    else:
        raise UnsupportedTypeError("unsupported C data type code '%d'" % c_data_type)

def _export_array(data, c_variable):
    if c_variable.data_type != _lib.harp_type_string:
        # NB. The _ffi.buffer() method as well as the numpy.frombuffer() method provide a view on the C array; neither
        # method incurs a copy. The numpy.copyto() method also works if the source array is a scalar, i.e. not an
        # instance of numpy.ndarray.
        size = c_variable.num_elements * _lib.harp_get_size_for_type(c_variable.data_type)
        shape = data.shape if isinstance(data, numpy.ndarray) else ()

        c_data_buffer = _ffi.buffer(c_variable.data.ptr, size)
        c_data = numpy.reshape(numpy.frombuffer(c_data_buffer, dtype=_get_py_data_type(c_variable.data_type)), shape)
        numpy.copyto(c_data, data, casting="safe")
    elif isinstance(data, numpy.ndarray):
        for index, element in enumerate(data.flat):
            if _lib.harp_variable_set_string_data_element(c_variable, index, _encode_string(element)) != 0:
                raise CLibraryError()
    else:
        assert(c_variable.num_elements == 1)
        if _lib.harp_variable_set_string_data_element(c_variable, 0, _encode_string(data)) != 0:
            raise CLibraryError()

def _export_variable(name, variable, c_product):
    data = getattr(variable, "data", None)
    if data is None:
        raise Error("no data or data is None")

    dimension = getattr(variable, "dimension", [])
    if not dimension and isinstance(data, numpy.ndarray) and data.size != 1:
        raise Error("dimensions missing or incomplete")

    if dimension and (not isinstance(data, numpy.ndarray) or data.ndim != len(dimension)):
        raise Error("dimensions incorrect")

    # Encode variable name.
    c_name = _encode_string(name)

    # Determine C dimension types and lengths.
    c_num_dimensions = len(dimension)
    c_dimension_type = [_get_c_dimension_type(dimension_name) for dimension_name in dimension]
    c_dimension = _ffi.NULL if not dimension else data.shape

    # Determine C data type.
    c_data_type = _get_c_data_type(data)

    # Create C variable of the proper size.
    c_variable_ptr = _ffi.new("harp_variable **")
    if _lib.harp_variable_new(c_name, c_data_type, c_num_dimensions, c_dimension_type, c_dimension,
                              c_variable_ptr) != 0:
        raise CLibraryError()

    # Add C variable to C product.
    if _lib.harp_product_add_variable(c_product, c_variable_ptr[0]) != 0:
        _lib.harp_variable_delete(c_variable_ptr[0])
        raise CLibraryError()

    # The C variable has been successfully added to the C product. Therefore, the memory management of the C variable is
    # tied to the life time of the C product. If an error occurs, the memory occupied by the C variable will be freed
    # along with the C product.
    c_variable = c_variable_ptr[0]

    # Copy data into the C variable.
    _export_array(data, c_variable)

    # Variable attributes.
    if c_data_type != _lib.harp_type_string:
        try:
            valid_min = variable.valid_min
        except AttributeError:
            pass
        else:
            if isinstance(valid_min, numpy.ndarray):
                if valid_min.size == 1:
                    valid_min = valid_min.flat[0]
                else:
                    raise Error("valid_min attribute should be scalar")

            c_data_type_valid_min = _get_c_data_type(valid_min)
            if _c_can_cast(c_data_type_valid_min, c_data_type):
                _export_scalar(valid_min, c_data_type, c_variable.valid_min)
            else:
                raise Error("type '%s' of valid_min attribute incompatible with type '%s' of data"
                            % (_get_c_data_type_name(c_data_type_valid_min), _get_c_data_type_name(c_data_type)))

        try:
            valid_max = variable.valid_max
        except AttributeError:
            pass
        else:
            if isinstance(valid_max, numpy.ndarray):
                if valid_max.size == 1:
                    valid_max = valid_max.flat[0]
                else:
                    raise Error("valid_max attribute should be scalar")

            c_data_type_valid_max = _get_c_data_type(valid_max)
            if _c_can_cast(c_data_type_valid_max, c_data_type):
                _export_scalar(valid_max, c_data_type, c_variable.valid_max)
            else:
                raise Error("type '%s' of valid_max attribute incompatible with type '%s' of data"
                            % (_get_c_data_type_name(c_data_type_valid_max), _get_c_data_type_name(c_data_type)))

    try:
        unit = variable.unit
    except AttributeError:
        pass
    else:
        if unit and _lib.harp_variable_set_unit(c_variable, _encode_string(unit)) != 0:
            raise CLibraryError()

    try:
        description = variable.description
    except AttributeError:
        pass
    else:
        if description and _lib.harp_variable_set_description(c_variable, _encode_string(description)) != 0:
            raise CLibraryError()

def _export_product(product, c_product):
    # Export product attributes.
    try:
        source_product = product.source_product
    except AttributeError:
        pass
    else:
        if source_product and _lib.harp_product_set_source_product(c_product, _encode_string(source_product)) != 0:
            raise CLibraryError()

    try:
        history = product.history
    except AttributeError:
        pass
    else:
        if history and _lib.harp_product_set_history(c_product, _encode_string(history)) != 0:
            raise CLibraryError()

    # Export variables.
    for name in product:
        try:
            _export_variable(name, product[name], c_product)
        except Error as _error:
            _error.args = (_error.args[0] + " (variable %r)" % name,) + _error.args[1:]
            raise

def get_encoding():
    """Return the encoding used to convert between unicode strings and C strings
    (only relevant when using Python 3).

    """
    return _encoding

def set_encoding(encoding):
    """Set the encoding used to convert between unicode strings and C strings
    (only relevant when using Python 3).

    """
    global _encoding

    _encoding = encoding

def version():
    """Return the version of the HARP C library."""
    return _decode_string(_ffi.string(_lib.libharp_version))

def to_dict(product):
    """Convert a Product to an OrderedDict.

    The OrderedDict representation provides direct access to the data associated
    with each variable. All product attributes and all variable attributes
    except the unit attribute are discarded as part of the conversion.

    The unit attribute of a variable is represented by adding a scalar variable
    of type string with the name of the corresponding variable suffixed with
    '_unit' as name and the unit as value.

    Arguments:
    product -- Product to convert.

    """
    if not isinstance(product, Product):
        raise TypeError("product must be Product, not %r" % product.__class__.__name__)

    dictionary = OrderedDict()

    for name in product:
        variable = product[name]

        try:
            dictionary[name] = variable.data
            if variable.unit:
                dictionary[name + "_unit"] = variable.unit
        except AttributeError:
            pass

    return dictionary

def import_product(filename, actions=""):
    """Import a HARP compliant product.

    The file format (NetCDF/HDF4/HDF5) of the product will be auto-detected.

    Arguments:
    filename -- Filename of the product to import.
    actions  -- Actions to execute on the product after it has been imported; should
                be specified as a semi-colon separated string of actions.

    """
    c_product_ptr = _ffi.new("harp_product **")

    # Import the product as a C product.
    if _lib.harp_import(_encode_path(filename), c_product_ptr) != 0:
        raise CLibraryError()

    try:
        # Execute action list expression on the C product.
        if actions and _lib.harp_product_execute_actions(c_product_ptr[0], _encode_string(actions)) != 0:
            raise CLibraryError()

        # Raise an exception if the imported C product contains no variables, or variables without data.
        if _lib.harp_product_is_empty(c_product_ptr[0]) == 1:
            raise NoDataError()

        # Convert the C product into its Python representation.
        return _import_product(c_product_ptr[0])

    finally:
        _lib.harp_product_delete(c_product_ptr[0])

def ingest_product(filename, actions="", options=""):
    """Ingest a product of a type supported by HARP.

    Arguments:
    filename -- Filename of the product to ingest.
    actions  -- Actions to execute as part of the ingestion; should be specified as a
                semi-colon separated string of actions.
    options  -- Ingestion module specific options; should be specified as a semi-
                colon separated string of key=value pairs.

    """
    c_product_ptr = _ffi.new("harp_product **")

    # Ingest the product as a C product.
    if _lib.harp_ingest(_encode_path(filename), _encode_string(actions), _encode_string(options), c_product_ptr) != 0:
        raise CLibraryError()

    try:
        # Raise an exception if the ingested C product contains no variables, or variables without data.
        if _lib.harp_product_is_empty(c_product_ptr[0]) == 1:
            raise NoDataError()

        # Convert the C product into its Python representation.
        return _import_product(c_product_ptr[0])

    finally:
        _lib.harp_product_delete(c_product_ptr[0])

def export_product(product, filename, file_format="netcdf"):
    """Export a HARP compliant product.

    Arguments:
    product     -- Product to export.
    filename    -- Filename of the exported product.
    file_format -- File format to use; one of 'netcdf', 'hdf4', or 'hdf5'.

    """
    if not isinstance(product, Product):
        raise TypeError("product must be Product, not %r" % product.__class__.__name__)

    # Create C product.
    c_product_ptr = _ffi.new("harp_product **")
    if _lib.harp_product_new(c_product_ptr) != 0:
        raise CLibraryError()

    try:
        # Convert the Python product to its C representation.
        _export_product(product, c_product_ptr[0])

        # Export the C product to a file.
        if _lib.harp_export(_encode_path(filename), _encode_string(file_format), c_product_ptr[0]) != 0:
            raise CLibraryError()

    finally:
        _lib.harp_product_delete(c_product_ptr[0])

#
# Initialize the HARP Python interface.
#
_init()