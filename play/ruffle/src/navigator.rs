//! Ruffle's network, answered from the game's own files. Nothing leaves the handheld: a file the pack lacks is a
//! 404, a page the game opens (a sponsor's site, "more games") is ignored, and sockets never connect.

use std::borrow::Cow;
use std::sync::Arc;
use std::time::Duration;

use async_channel::{Receiver, Sender};
use ruffle_core::backend::navigator::{
    ErrorResponse, NavigationMethod, NavigatorBackend, NullSpawner, OwnedFuture, Request, SuccessResponse,
    async_return, create_specific_fetch_error,
};
use ruffle_core::indexmap::IndexMap;
use ruffle_core::loader::Error;
use ruffle_core::socket::{ConnectionState, SocketAction, SocketHandle};
use url::{ParseError, Url};

use crate::content::Content;

pub struct PackNavigator {
    content: Arc<Content>,
    spawner: NullSpawner,
}

impl PackNavigator {
    pub fn new(content: Arc<Content>, spawner: NullSpawner) -> Self {
        Self { content, spawner }
    }
}

struct PackResponse {
    url: String,
    body: Option<Vec<u8>>,
}

impl SuccessResponse for PackResponse {
    fn url(&self) -> Cow<'_, str> {
        Cow::Borrowed(&self.url)
    }
    fn set_url(&mut self, url: String) {
        self.url = url;
    }
    fn body(self: Box<Self>) -> OwnedFuture<Vec<u8>, Error> {
        Box::pin(async move { Ok(self.body.unwrap_or_default()) })
    }
    fn text_encoding(&self) -> Option<&'static encoding_rs::Encoding> {
        None
    }
    fn status(&self) -> u16 {
        200
    }
    fn redirected(&self) -> bool {
        false
    }
    fn next_chunk(&mut self) -> OwnedFuture<Option<Vec<u8>>, Error> {
        let chunk = self.body.take();
        Box::pin(async move { Ok(chunk) })
    }
    fn expected_length(&self) -> Result<Option<u64>, Error> {
        Ok(self.body.as_ref().map(|b| b.len() as u64))
    }
}

impl NavigatorBackend for PackNavigator {
    fn navigate_to_url(
        &self,
        url: &str,
        _target: &str,
        _vars_method: Option<(NavigationMethod, IndexMap<String, String>)>,
    ) {
        tracing::info!("flash: the game opened {url} (ignored, offline)");
    }

    fn fetch(&self, request: Request) -> OwnedFuture<Box<dyn SuccessResponse>, ErrorResponse> {
        let raw = request.url().to_string();
        let url = match self.resolve_url(&raw) {
            Ok(u) => u,
            Err(e) => return async_return(Err(create_specific_fetch_error("Invalid URL", &raw, e))),
        };
        match self.content.read(&url) {
            Some(body) => {
                let response: Box<dyn SuccessResponse> =
                    Box::new(PackResponse { url: url.to_string(), body: Some(body) });
                async_return(Ok(response))
            }
            None => {
                tracing::warn!("flash: {url} is not in the game's files");
                async_return(Err(create_specific_fetch_error("Not in the game's files", url.as_str(), "")))
            }
        }
    }

    fn resolve_url(&self, url: &str) -> Result<Url, ParseError> {
        match Url::parse(url) {
            Ok(u) => Ok(u),
            Err(ParseError::RelativeUrlWithoutBase) => Url::parse(&self.content.launch)?.join(url),
            Err(e) => Err(e),
        }
    }

    fn spawn_future(&mut self, future: OwnedFuture<(), Error>) {
        self.spawner.spawn_local(future);
    }

    fn pre_process_url(&self, url: Url) -> Url {
        url
    }

    fn connect_socket(
        &mut self,
        _host: String,
        _port: u16,
        _timeout: Duration,
        handle: SocketHandle,
        _receiver: Receiver<Vec<u8>>,
        sender: Sender<SocketAction>,
    ) {
        let _ = sender.try_send(SocketAction::Connect(handle, ConnectionState::Failed));
    }
}
