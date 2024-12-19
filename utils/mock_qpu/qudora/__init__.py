# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import cudaq
from fastapi import FastAPI, HTTPException, Header
from typing import Union
import uvicorn, uuid, base64, ctypes
from pydantic import BaseModel
from llvmlite import binding as llvm
import json

# Define the REST Server App
app = FastAPI()


class InputJob(BaseModel):
    name: str
    language: str
    shots: list[int]
    target: str
    input_data: list[str]
    backend_settings: str | None


# Jobs look like the following type
class JobStatus(BaseModel):
    job_id: int
    name: str
    result: list[str]
    shots: list[int]
    status: str
    target: str


# Keep track of Job Ids to their Names
createdJobs = {}

# Could how many times the client has requested the Job
countJobGetRequests = 0

# Save how many qubits were needed for each test (emulates real backend)
numQubitsRequired = 0

llvm.initialize()
llvm.initialize_native_target()
llvm.initialize_native_asmprinter()
target = llvm.Target.from_default_triple()
targetMachine = target.create_target_machine()
backing_mod = llvm.parse_assembly("")
engine = llvm.create_mcjit_compiler(backing_mod, targetMachine)


def getKernelFunction(module):
    for f in module.functions:
        if not f.is_declaration:
            return f
    return None


def getNumRequiredQubits(function):
    for a in function.attributes:
        if "requiredQubits" in str(a):
            return int(
                str(a).split("requiredQubits\"=")[-1].split(" ")[0].replace(
                    "\"", ""))

def simulate_qir(program: str, shots: int):
    decoded = base64.b64decode(program)
    m = llvm.module.parse_bitcode(decoded)
    mstr = str(m)
    assert ('entry_point' in mstr)

    # Get the function, number of qubits, and kernel name
    function = getKernelFunction(m)
    if function == None:
        raise Exception("Could not find kernel function")
    numQubitsRequired = getNumRequiredQubits(function)
    kernelFunctionName = function.name

    print("Kernel name = ", kernelFunctionName)
    print("Requires {} qubits".format(numQubitsRequired))
    print(mstr)

    # JIT Compile and get Function Pointer
    engine.add_module(m)
    engine.finalize_object()
    engine.run_static_constructors()
    funcPtr = engine.get_function_address(kernelFunctionName)
    kernel = ctypes.CFUNCTYPE(None)(funcPtr)

    # Invoke the Kernel
    cudaq.testing.toggleDynamicQubitManagement()
    qubits, context = cudaq.testing.initialize(numQubitsRequired, shots)
    print("Calling kernel!")
    kernel()
    results = cudaq.testing.finalize(qubits, context)
    
    result_dict = {bits: counts for bits, counts in results.items()}
    print(result_dict)
    engine.remove_module(m)

    return json.dumps(result_dict)


# Here we expose a way to post jobs,
# Must have a Access Token, Job Program must be Adaptive Profile
# with entry_point tag
@app.post("/jobs")
async def postJob(job: InputJob,
                  token: Union[str, None] = Header(alias="Authorization",
                                                   default=None)):
    global createdJobs, numQubitsRequired

    if token == None:
        raise HTTPException(401, detail="Credentials not provided")

    print('Posting job with shots = ', job.shots)
    newId = str(uuid.uuid4())

    results = [simulate_qir(program, shots) for program, shots in zip(job.input_data, job.shots)]

    createdJobs[newId] = results
    print("Adding job results to id", newId)

    # Job "created", return the id
    return newId


# Retrieve the job, simulate having to wait by counting to 3
# until we return the job results
@app.get("/jobs")
async def getJob(job_id: str, include_results: bool):
    global countJobGetRequests, createdJobs, numQubitsRequired

    assert include_results, "include_results=False not implemented."

    # Simulate asynchronous execution
    if countJobGetRequests < 3:
        countJobGetRequests += 1
        return [{"status": "Running"}]

    countJobGetRequests = 0

    job_id = json.loads(job_id)

    print("Requesting job status for id", job_id)

    res = [{
        "status": "Completed",
        "result": createdJobs[job_id]
    }]
    print("Returning result:", res)
    return res


def startServer(port):
    uvicorn.run(app, port=port, host='0.0.0.0', log_level="info")


if __name__ == '__main__':
    startServer(8100)
