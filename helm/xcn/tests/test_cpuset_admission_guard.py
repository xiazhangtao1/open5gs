#!/usr/bin/env python3

import importlib.util
import json
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "files" / "cpuset-admission-guard.py"
SPEC = importlib.util.spec_from_file_location("cpuset_admission_guard", MODULE_PATH)
GUARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARD)


class AllocationReadyTest(unittest.TestCase):
    def test_accepts_complete_allocation(self):
        value = json.dumps({
            "upf": {"cpu": "45-46,117-118"},
            "vpp": {"cpu": "47,119"},
            "smf": {"cpu": "48"},
        })
        self.assertTrue(GUARD.allocation_ready(value, ["upf", "vpp", "smf"]))

    def test_rejects_missing_container(self):
        value = json.dumps({"upf": {"cpu": "45-46,117-118"}})
        self.assertFalse(GUARD.allocation_ready(value, ["upf", "vpp", "smf"]))

    def test_rejects_invalid_cpuset(self):
        value = json.dumps({
            "upf": {"cpu": "45-46,117-118"},
            "vpp": {"cpu": "119-47"},
            "smf": {"cpu": "48"},
        })
        self.assertFalse(GUARD.allocation_ready(value, ["upf", "vpp", "smf"]))


class DeletePodTest(unittest.TestCase):
    def test_deletes_pod_with_bearer_token(self):
        response = mock.MagicMock()
        response.__enter__.return_value.status = 202
        with mock.patch.object(
                GUARD.ssl, "create_default_context") as context, mock.patch.object(
                    GUARD.urllib.request, "urlopen", return_value=response) as open_url:
            GUARD.delete_pod(
                "https://kubernetes/api/v1/namespaces/xcn/pods/xcn-5gc",
                "token-value", Path("/ca.crt"), 7)

        context.assert_called_once_with(cafile="/ca.crt")
        request = open_url.call_args.args[0]
        self.assertEqual(request.method, "DELETE")
        self.assertEqual(request.get_header("Authorization"), "Bearer token-value")
        self.assertEqual(open_url.call_args.kwargs["timeout"], 7)

    def test_treats_already_deleted_pod_as_success(self):
        error = GUARD.urllib.error.HTTPError(
            "https://kubernetes/pod", 404, "Not Found", {}, None)
        with mock.patch.object(
                GUARD.ssl, "create_default_context"), mock.patch.object(
                    GUARD.urllib.request, "urlopen", side_effect=error):
            GUARD.delete_pod(
                "https://kubernetes/pod", "token", Path("/ca.crt"), 5)


if __name__ == "__main__":
    unittest.main()
