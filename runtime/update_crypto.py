"""Ed25519 verification via the stock OpenSSL EVP API; no custom cryptography."""
import ctypes as c
import ctypes.util
import sys

def verify(public, signature, message):
    if len(public)!=32 or len(signature)!=64: return False
    name=ctypes.util.find_library('crypto')
    if sys.platform=='darwin' and not name: name='/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib'
    lib=c.CDLL(name or 'libcrypto.so.3')
    def api(name,restype,args):
        f=getattr(lib,name);f.restype=restype;f.argtypes=args;return f
    newkey=api('EVP_PKEY_new_raw_public_key_ex',c.c_void_p,[c.c_void_p,c.c_char_p,c.c_char_p,c.c_void_p,c.c_size_t])
    freekey=api('EVP_PKEY_free',None,[c.c_void_p]);newctx=api('EVP_MD_CTX_new',c.c_void_p,[]);freectx=api('EVP_MD_CTX_free',None,[c.c_void_p])
    init=api('EVP_DigestVerifyInit',c.c_int,[c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p])
    check=api('EVP_DigestVerify',c.c_int,[c.c_void_p,c.c_void_p,c.c_size_t,c.c_void_p,c.c_size_t])
    key=newkey(None,b'ED25519',None,public,len(public));ctx=newctx()
    try:
        return bool(key and ctx and init(ctx,None,None,None,key)==1 and check(ctx,signature,len(signature),message,len(message))==1)
    finally:
        if ctx:freectx(ctx)
        if key:freekey(key)
