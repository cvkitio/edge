# EMD Agent TODO

## High Priority

### Clip Download/Playback Issues
**Status**: Investigating  
**Date**: 2026-05-18  

**Issue**: Downloaded clips via API endpoint appear corrupted when checked with ffprobe:
```
ffprobe: End of file
ffmpeg: could not find codec parameters
```

**What we know**:
- Timestamp normalization IS working correctly (confirmed via debug logs)
- Clips are being created with correct durations (~20 seconds)
- File sizes look reasonable (1-2 MB per clip)
- Debug output shows: `norm_pts=0 norm_dts=0` for each clip
- Issue occurs when downloading via `/api/clips/{camera}/{filename}` endpoint

**Possible causes**:
1. HTTP streaming/range request issue in the API handler
2. Timing issue - clips being downloaded while still being written
3. File corruption during transfer
4. Issue with how clips are being served (need to verify Content-Type headers)

**Next steps**:
- Test direct file access in pod vs API endpoint
- Check HTTP headers in clip serving endpoint
- Verify fsync behavior is completing before serving
- Test with multiple browsers/tools (curl, wget, browser)

---

## Future Enhancements

### Security
- [ ] Rotate/revoke GitHub token after deployment (***REDACTED_GITHUB_TOKEN***)
- [ ] Use sealed secrets or external secrets operator for production

### MPEG-TS Muxer
- [ ] Remove debug logging once playback is fully validated
- [ ] Consider making timestamp normalization configurable (though always-on is safer for browsers)

### Configuration
- [ ] Implement tomlc99 array parsing for target_classes feature (requires full TOML library)
- [ ] Alternative: switch to a different TOML parser that supports arrays

### Monitoring
- [ ] Add Prometheus metrics for clip creation success/failure rates
- [ ] Add metrics for timestamp normalization deltas
- [ ] Alert on clips with unusual timestamp patterns
